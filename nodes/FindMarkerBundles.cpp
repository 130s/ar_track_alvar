/*
  Software License Agreement (BSD License)

  Copyright (c) 2012, Scott Niekum
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.
  * Neither the name of the Willow Garage nor the names of its
  contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

  author: Scott Niekum
*/


#include "CvTestbed.h"
#include "MarkerDetector.h"
#include "MultiMarkerBundle.h"
#include "MultiMarkerInitializer.h"
#include "Shared.h"
#include <cv_bridge/CvBridge.h>
#include <ar_track_alvar/AlvarMarker.h>
#include <ar_track_alvar/AlvarMarkers.h>
#include <tf/transform_listener.h>

#include <sensor_msgs/PointCloud2.h>
#include <pcl/ros/conversions.h>
#include <pcl/point_types.h>
#include <pcl/registration/icp.h>
#include <pcl/registration/registration.h>

#include <geometry_msgs/PoseStamped.h>
#include <ros/ros.h>
#include <pcl/ModelCoefficients.h>
#include <pcl/point_types.h>
#include <pcl/sample_consensus/method_types.h>
#include <pcl/sample_consensus/model_types.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/filters/extract_indices.h>
#include <boost/lexical_cast.hpp>

#include <LinearMath/btMatrix3x3.h>
#include <ar_track_alvar/kinect_filtering.h>

#define MAIN_MARKER 1
#define VISIBLE_MARKER 2
#define GHOST_MARKER 3

using std::cerr;

namespace gm=geometry_msgs;
namespace ata=ar_track_alvar;

typedef pcl::PointXYZRGB ARPoint;
typedef pcl::PointCloud<ARPoint> ARCloud;

using namespace alvar;
using namespace std;
using boost::make_shared;

// Pixel coordinates in an image
struct Pixel
{
  unsigned r, c;
  Pixel (unsigned r=0, unsigned c=0) : r(r), c(c) {}
};

// Result of plane fit: inliers and the plane equation
struct PlaneFitResult
{
  PlaneFitResult () : inliers(make_shared<ARCloud>()) {}
  ARCloud::Ptr inliers;
  pcl::ModelCoefficients coeffs;
};

Camera *cam;
IplImage *capture_;
sensor_msgs::CvBridge bridge_;
image_transport::Subscriber cam_sub_;
ros::Subscriber cloud_sub_;
ros::Publisher arMarkerPub_;
ros::Publisher rvizMarkerPub_;
ros::Publisher rvizMarkerPub2_;
ar_track_alvar::AlvarMarkers arPoseMarkers_;
tf::TransformListener *tf_listener;
tf::TransformBroadcaster *tf_broadcaster;
MarkerDetector<MarkerData> marker_detector;
MultiMarkerBundle **multi_marker_bundles=NULL;

Pose *bundlePoses;
int *master_id;
bool *bundles_seen;
bool *master_visible;
std::vector<int> *bundle_indices; 	
bool init = true;

int median_n;  
Pose **median_poses;
int *median_ind;
bool *median_init;

double marker_size;
double max_new_marker_error;
double max_track_error;
std::string cam_image_topic; 
std::string cam_info_topic; 
std::string output_frame;
int n_bundles = 0;   

//Debugging utility function
void draw3dPoints(ARCloud::Ptr cloud, string frame, int color, int id, double rad)
{
  visualization_msgs::Marker rvizMarker;

  rvizMarker.header.frame_id = frame;
  rvizMarker.header.stamp = ros::Time::now(); 
  rvizMarker.id = id;
  rvizMarker.ns = "3dpts";
  
  rvizMarker.scale.x = rad;
  rvizMarker.scale.y = rad;
  rvizMarker.scale.z = rad;
  
  rvizMarker.type = visualization_msgs::Marker::SPHERE_LIST;
  rvizMarker.action = visualization_msgs::Marker::ADD;
  
  if(color==1){
    rvizMarker.color.r = 0.0f;
    rvizMarker.color.g = 1.0f;
    rvizMarker.color.b = 1.0f;
    rvizMarker.color.a = 1.0;
  }
  if(color==2){
    rvizMarker.color.r = 1.0f;
    rvizMarker.color.g = 0.0f;
    rvizMarker.color.b = 1.0f;
    rvizMarker.color.a = 1.0;
  }
  if(color==3){
    rvizMarker.color.r = 1.0f;
    rvizMarker.color.g = 1.0f;
    rvizMarker.color.b = 0.0f;
    rvizMarker.color.a = 1.0;
  }
  
  gm::Point p;
  for(int i=0; i<cloud->points.size(); i++){
    p.x = cloud->points[i].x;
    p.y = cloud->points[i].y;
    p.z = cloud->points[i].z;
    rvizMarker.points.push_back(p);
  }
  
  rvizMarker.lifetime = ros::Duration (1.0);
  rvizMarkerPub2_.publish (rvizMarker);
}


void drawArrow(gm::Point start, btMatrix3x3 mat, string frame, int color, int id)
{
  visualization_msgs::Marker rvizMarker;
  
  rvizMarker.header.frame_id = frame;
  rvizMarker.header.stamp = ros::Time::now(); 
  rvizMarker.id = id;
  rvizMarker.ns = "arrow";
  
  rvizMarker.scale.x = 0.01;
  rvizMarker.scale.y = 0.01;
  rvizMarker.scale.z = 0.1;
  
  rvizMarker.type = visualization_msgs::Marker::ARROW;
  rvizMarker.action = visualization_msgs::Marker::ADD;
  
  for(int i=0; i<3; i++){
    rvizMarker.points.clear();	
    rvizMarker.points.push_back(start);
    gm::Point end;
    end.x = start.x + mat[0][i];
    end.y = start.y + mat[1][i];
    end.z = start.z + mat[2][i];
    rvizMarker.points.push_back(end);
    rvizMarker.id += 10*i;
    rvizMarker.lifetime = ros::Duration (1.0);

    if(color==1){
      rvizMarker.color.r = 1.0f;
      rvizMarker.color.g = 0.0f;
      rvizMarker.color.b = 0.0f;
      rvizMarker.color.a = 1.0;
    }
    if(color==2){
      rvizMarker.color.r = 0.0f;
      rvizMarker.color.g = 1.0f;
      rvizMarker.color.b = 0.0f;
      rvizMarker.color.a = 1.0;
    }
    if(color==3){
      rvizMarker.color.r = 0.0f;
      rvizMarker.color.g = 0.0f;
      rvizMarker.color.b = 1.0f;
      rvizMarker.color.a = 1.0;
    }
    color += 1;

    rvizMarkerPub2_.publish (rvizMarker);
  }
}


// Infer the master tag corner positons from the other observed tags
// Also does some of the bookkeeping for tracking that MultiMarker::_GetPose does in the non-kinect case
int InferCorners(const ARCloud &cloud, MultiMarkerBundle &master, ARCloud &bund_corners){
  bund_corners.clear();
  bund_corners.resize(4);
  for(int i=0; i<4; i++){
    bund_corners[i].x = 0;
    bund_corners[i].y = 0;
    bund_corners[i].z = 0;
  }

  // Reset the marker_status to 1 for all markers in point_cloud
  for (size_t i=0; i<master.marker_status.size(); i++) {
    if (master.marker_status[i] > 0) master.marker_status[i]=1;
  }

  int n_est = 0;

  // For every detected marker
  for (size_t i=0; i<marker_detector.markers->size(); i++)
    {
      const Marker* marker = &((*marker_detector.markers)[i]);
      int id = marker->GetId();
      int index = master.get_id_index(id);
      if (index < 0) continue;

      // But only if we have corresponding points in the pointcloud
      if (master.marker_status[index] > 0) {
	n_est++;

	std::string marker_frame = "ar_marker_";
	std::stringstream out;
	out << id;
	std::string id_string = out.str();
	marker_frame += id_string;

	for(int j = 0; j < marker->marker_corners.size(); ++j)
	  {
	    //Get the estimated coords of the master marker corner in the cam frame and average the estimates 
	    //Note: the coords of the master tag in marker frame are just the neg coords of the marker in master frame, except opposite corners
	    CvPoint3D64f Xnew = master.pointcloud[master.pointcloud_index(id, (int)j)];
	    gm::PointStamped p, output_p;
	    p.point.y = Xnew.x / 100.0;
	    p.point.x = -Xnew.y / 100.0;
	    p.point.z = Xnew.z / 100.0;
	    p.header.frame_id = marker_frame; 	
	    p.header.stamp = ros::Time(0);

	    try{
	      tf_listener->waitForTransform(cloud.header.frame_id, marker_frame, ros::Time(0), ros::Duration(0.1));
	      tf_listener->transformPoint(cloud.header.frame_id, p, output_p);			
	    }
	    catch (tf::TransformException ex){
	      ROS_ERROR("%s",ex.what());
	      return -1;
	    }

	    //Account for the diagonal "corner switching" that occurs when calculating the master corners from the current marker corners
	    int opp_ind = (j+2)%4;
	    bund_corners[opp_ind].x += output_p.point.x;
	    bund_corners[opp_ind].y += output_p.point.y;
	    bund_corners[opp_ind].z += output_p.point.z;
	    //printf("corner %i    %f %f %f   |   %f %f %f\n",j,p.point.x,p.point.y,p.point.z,output_p.point.x,output_p.point.y,output_p.point.z);  
	  }
	master.marker_status[index] = 2; // Used for tracking
      }
    }
  
  //Divide to take the average of the summed estimates
  for(int i=0; i<4; i++){
    bund_corners[i].x /= n_est;
    bund_corners[i].y /= n_est;
    bund_corners[i].z /= n_est;
    //cout << "Infer corners " << i << ": " << bund_corners[i].x << " " << bund_corners[i].y << " " << bund_corners[i].z << " " << endl;
  }

  return 0;
}


int PlaneFitPoseImprovement(int id, const ARCloud &corners_3D, ARCloud::Ptr selected_points, const ARCloud &cloud, Pose &p){

  ata::PlaneFitResult res = ata::fitPlane(selected_points);
  gm::PoseStamped pose;
  pose.header.stamp = cloud.header.stamp;
  pose.header.frame_id = cloud.header.frame_id;
  pose.pose.position = ata::centroid(*res.inliers);

  draw3dPoints(selected_points, cloud.header.frame_id, 1, id, 0.005);
	  
  //Get 2 points that point forward in marker x direction   
  int i1,i2;
  if(isnan(corners_3D[0].x) || isnan(corners_3D[0].y) || isnan(corners_3D[0].z) || 
     isnan(corners_3D[3].x) || isnan(corners_3D[3].y) || isnan(corners_3D[3].z))
    {
      if(isnan(corners_3D[1].x) || isnan(corners_3D[1].y) || isnan(corners_3D[1].z) || 
	 isnan(corners_3D[2].x) || isnan(corners_3D[2].y) || isnan(corners_3D[2].z))
	{
	  return -1;
	}
      else{
	i1 = 1;
	i2 = 2;
      }	
    }
  else{
    i1 = 0;
    i2 = 3;
  }

  //Get 2 points the point forward in marker y direction   
  int i3,i4;
  if(isnan(corners_3D[0].x) || isnan(corners_3D[0].y) || isnan(corners_3D[0].z) || 
     isnan(corners_3D[1].x) || isnan(corners_3D[1].y) || isnan(corners_3D[1].z))
    {
      if(isnan(corners_3D[3].x) || isnan(corners_3D[3].y) || isnan(corners_3D[3].z) || 
	 isnan(corners_3D[2].x) || isnan(corners_3D[2].y) || isnan(corners_3D[2].z))
	{
	  return -1;
	}
      else{
	i3 = 2;
	i4 = 3;
      }	
    }
  else{
    i3 = 1;
    i4 = 0;
  }
   
  ARCloud::Ptr orient_points(new ARCloud());
  orient_points->points.push_back(corners_3D[i1]);
  draw3dPoints(orient_points, cloud.header.frame_id, 3, id+1000, 0.008);
      
  orient_points->clear();
  orient_points->points.push_back(corners_3D[i2]);
  draw3dPoints(orient_points, cloud.header.frame_id, 2, id+2000, 0.008);
 
  int succ;
  succ = ata::extractOrientation(res.coeffs, corners_3D[i1], corners_3D[i2], corners_3D[i3], corners_3D[i4], pose.pose.orientation);
  if(succ < 0) return -1;

  btMatrix3x3 mat; 
  succ = ata::extractFrame(res.coeffs, corners_3D[i1], corners_3D[i2], corners_3D[i3], corners_3D[i4], mat);
  if(succ < 0) return -1;

  drawArrow(pose.pose.position, mat, cloud.header.frame_id, 1, id);

  p.translation[0] = pose.pose.position.x * 100.0;
  p.translation[1] = pose.pose.position.y * 100.0;
  p.translation[2] = pose.pose.position.z * 100.0;
  p.quaternion[1] = pose.pose.orientation.x;
  p.quaternion[2] = pose.pose.orientation.y;
  p.quaternion[3] = pose.pose.orientation.z;
  p.quaternion[0] = pose.pose.orientation.w; 

  return 0;
}


void medianFilter(int bund, const Pose &new_pose, Pose &ret_pose){
  median_poses[bund][median_ind[bund]] = new_pose;
  
  if(!median_init[bund]){
    if(median_ind[bund] == (median_n-1))
      median_init[bund] = true;
    ret_pose = new_pose;
  }

  else{
    double min_dist = 0;
    int min_ind = 0;
    for(int i=0; i<median_n; i++){
      double total_dist = 0;
      for(int j=0; j<median_n; j++){
	total_dist += pow(median_poses[bund][i].translation[0] - median_poses[bund][j].translation[0], 2);
	total_dist += pow(median_poses[bund][i].translation[1] - median_poses[bund][j].translation[1], 2);
	total_dist += pow(median_poses[bund][i].translation[2] - median_poses[bund][j].translation[2], 2);
	total_dist += pow(median_poses[bund][i].quaternion[0] - median_poses[bund][j].quaternion[0], 2);
	total_dist += pow(median_poses[bund][i].quaternion[1] - median_poses[bund][j].quaternion[1], 2);
	total_dist += pow(median_poses[bund][i].quaternion[2] - median_poses[bund][j].quaternion[2], 2);
	total_dist += pow(median_poses[bund][i].quaternion[3] - median_poses[bund][j].quaternion[3], 2);
      }
      if(i==0) min_dist = total_dist;
      else{
	if(total_dist < min_dist){
	  min_dist = total_dist;
	  min_ind = i;
	}
      }
    }
    ret_pose = median_poses[bund][min_ind];
    //cout << median_ind[bund] << " min " << min_dist << " " << min_ind << endl;
  }

  median_ind[bund] = (median_ind[bund]+1) % median_n;
}


// Updates the bundlePoses of the multi_marker_bundles by detecting markers and
// using all markers in a bundle to infer the master tag's position
void GetMultiMarkerPoses(IplImage *image, ARCloud &cloud) {

  for(int i=0; i<n_bundles; i++)
    master_visible[i] = false;

  for(int i=0; i<n_bundles; i++)
    bundles_seen[i] = false;

  //Detect and track the markers
  if (marker_detector.Detect(image, cam, true, false, max_new_marker_error,
			     max_track_error, CVSEQ, true)) 
    {
      printf("\n--------------------------\n\n");
      for (size_t i=0; i<marker_detector.markers->size(); i++)
    	{
	  vector<cv::Point> pixels;
	  Marker *m = &((*marker_detector.markers)[i]);
	  int id = m->GetId();
	  cout << "******* ID: " << id << endl;
      
	  //Get the 3D points of the outer corners
          /*
	  PointDouble corner0 = m->marker_corners_img[0];
	  PointDouble corner1 = m->marker_corners_img[1];
	  PointDouble corner2 = m->marker_corners_img[2];
	  PointDouble corner3 = m->marker_corners_img[3];
	  m->ros_corners_3D[0] = cloud(corner0.x, corner0.y);
	  m->ros_corners_3D[1] = cloud(corner1.x, corner1.y);
	  m->ros_corners_3D[2] = cloud(corner2.x, corner2.y);
	  m->ros_corners_3D[3] = cloud(corner3.x, corner3.y);
	  */
          
	  //Get the 3D inner corner points - more stable than outer corners that can "fall off" object
	  int resol = m->GetRes();
	  int ori = m->ros_orientation;
      
	  PointDouble pt1, pt2, pt3, pt4;
	  pt4 = m->ros_marker_points_img[0];
	  pt3 = m->ros_marker_points_img[resol-1];
	  pt1 = m->ros_marker_points_img[(resol*resol)-resol];
	  pt2 = m->ros_marker_points_img[(resol*resol)-1];
	  
	  m->ros_corners_3D[0] = cloud(pt1.x, pt1.y);
	  m->ros_corners_3D[1] = cloud(pt2.x, pt2.y);
	  m->ros_corners_3D[2] = cloud(pt3.x, pt3.y);
	  m->ros_corners_3D[3] = cloud(pt4.x, pt4.y);
	  
	  if(ori >= 0 && ori < 4){
	    if(ori != 0){
	      std::rotate(m->ros_corners_3D.begin(), m->ros_corners_3D.begin() + ori, m->ros_corners_3D.end());
	    }
	  }
	  else
	    ROS_ERROR("FindMarkerBundles: Bad Orientation: %i for ID: %i", ori, id);

	  //Check if we have spotted a master tag
	  int master_ind = -1;
	  for(int j=0; j<n_bundles; j++){
	    if(id == master_id[j])
	      master_visible[j] = true; 
	    master_ind = j;
	  }

	  //Mark the bundle that marker belongs to as "seen"
	  int bundle_ind = -1;
	  for(int j=0; j<n_bundles; j++){
	    for(int k=0; k<bundle_indices[j].size(); k++){
	      if(bundle_indices[j][k] == id){
		if(bundles_seen[j] == false)  //Check if it was already seen before
		  bundle_ind = j;
		bundles_seen[j] = true;
		break;
	      }
	    }
	  }

	  //Get the 3D marker points
	  BOOST_FOREACH (const PointDouble& p, m->ros_marker_points_img)
	    pixels.push_back(cv::Point(p.x, p.y));	  
	  ARCloud::Ptr selected_points = ata::filterCloud(cloud, pixels);

	  //Use the kinect data to find a plane and pose for the marker
	  int ret = PlaneFitPoseImprovement(i, m->ros_corners_3D, selected_points, cloud, m->pose);
            
	  //If the plane fit fails...
	  if(ret < 0){
	    //If this was a master tag, reset its visibility
	    if(master_ind >= 0)
	      master_visible[master_ind] = false;
	    //If this was the only observed tag in the bundle, the bundle is no longer "seen"
	    if(bundle_ind >= 0)
	      bundles_seen[bundle_ind];
	  }
	}	

      //For each master tag that isn't directly visible, infer the 3D position of its corners from other visible tags
      //Then, do a plane fit to those new corners   	
      ARCloud inferred_corners;
      for(int i=0; i<n_bundles; i++){
	if(bundles_seen[i] == true){
	  if(master_visible[i] == false){
	    if(InferCorners(cloud, *(multi_marker_bundles[i]), inferred_corners) >= 0){
	      ARCloud::Ptr inferred_cloud(new ARCloud(inferred_corners));
	      PlaneFitPoseImprovement(i+5000, inferred_corners, inferred_cloud, cloud, bundlePoses[i]);
	    }
	  }
	  //Otherwise, if we can see the master tag, just rely on the pose we found from the kinect data above 
	  else{
	    for (size_t j=0; j<marker_detector.markers->size(); j++){
	      Marker *m = &((*marker_detector.markers)[j]);                	
	      if(m->GetId() == master_id[i])
		bundlePoses[i] = m->pose;
	    } 
	  }
	  Pose ret_pose;
	  medianFilter(i,bundlePoses[i], ret_pose);
	  bundlePoses[i] = ret_pose;   
	}		
      }
    }
}


// Given the pose of a marker, builds the appropriate ROS messages for later publishing 
void makeMarkerMsgs(int type, int id, Pose &p, sensor_msgs::ImageConstPtr image_msg, tf::StampedTransform &CamToOutput, visualization_msgs::Marker *rvizMarker, ar_track_alvar::AlvarMarker *ar_pose_marker){
  double px,py,pz,qx,qy,qz,qw;
	
  px = p.translation[0]/100.0;
  py = p.translation[1]/100.0;
  pz = p.translation[2]/100.0;
  qx = p.quaternion[1];
  qy = p.quaternion[2];
  qz = p.quaternion[3];
  qw = p.quaternion[0];

  //Get the marker pose in the camera frame
  btQuaternion rotation (qx,qy,qz,qw);
  btVector3 origin (px,py,pz);
  btTransform t (rotation, origin);  //transform from cam to marker

  btVector3 markerOrigin (0, 0, 0);
  btTransform m (btQuaternion::getIdentity (), markerOrigin);
  btTransform markerPose = t * m;

  //Publish the cam to marker transform for each marker
  std::string markerFrame = "ar_marker_";
  std::stringstream out;
  out << id;
  std::string id_string = out.str();
  markerFrame += id_string;
  tf::StampedTransform camToMarker (t, image_msg->header.stamp, image_msg->header.frame_id, markerFrame.c_str());
  tf_broadcaster->sendTransform(camToMarker);

  //Create the rviz visualization message
  tf::poseTFToMsg (markerPose, rvizMarker->pose);
  rvizMarker->header.frame_id = image_msg->header.frame_id;
  rvizMarker->header.stamp = image_msg->header.stamp;
  rvizMarker->id = id;

  rvizMarker->scale.x = 1.0 * marker_size/100.0;
  rvizMarker->scale.y = 1.0 * marker_size/100.0;
  rvizMarker->scale.z = 0.2 * marker_size/100.0;

  if(type==MAIN_MARKER)
    rvizMarker->ns = "main_shapes";
  else
    rvizMarker->ns = "basic_shapes";


  rvizMarker->type = visualization_msgs::Marker::CUBE;
  rvizMarker->action = visualization_msgs::Marker::ADD;

  //Determine a color and opacity, based on marker type
  if(type==MAIN_MARKER){
    rvizMarker->color.r = 1.0f;
    rvizMarker->color.g = 0.0f;
    rvizMarker->color.b = 0.0f;
    rvizMarker->color.a = 1.0;
  }
  else if(type==VISIBLE_MARKER){
    rvizMarker->color.r = 0.0f;
    rvizMarker->color.g = 1.0f;
    rvizMarker->color.b = 0.0f;
    rvizMarker->color.a = 0.7;
  }
  else if(type==GHOST_MARKER){
    rvizMarker->color.r = 0.0f;
    rvizMarker->color.g = 0.0f;
    rvizMarker->color.b = 1.0f;
    rvizMarker->color.a = 0.5;
  }

  rvizMarker->lifetime = ros::Duration (0.1);

  // Only publish the pose of the master tag in each bundle, since that's all we really care about aside from visualization 
  if(type==MAIN_MARKER){
    //Take the pose of the tag in the camera frame and convert to the output frame (usually torso_lift_link for the PR2)
    tf::Transform tagPoseOutput = CamToOutput * markerPose;

    //Create the pose marker message
    tf::poseTFToMsg (tagPoseOutput, ar_pose_marker->pose.pose);
    ar_pose_marker->header.frame_id = output_frame;
    ar_pose_marker->header.stamp = image_msg->header.stamp;
    ar_pose_marker->id = id;
  }
  else
    ar_pose_marker = NULL;
}



//Callback to handle getting kinect point clouds and processing them
void getPointCloudCallback (const sensor_msgs::PointCloud2ConstPtr &msg)
{
  sensor_msgs::ImagePtr image_msg(new sensor_msgs::Image);

  if(init){
    CvSize sz_ = cvSize (cam->x_res, cam->y_res);
    capture_ = cvCreateImage (sz_, IPL_DEPTH_8U, 4);
    init = false;	
  }

  //If we've already gotten the cam info, then go ahead
  if(cam->getCamInfo_){
    try{
      //Get the transformation from the Camera to the output frame for this image capture
      tf::StampedTransform CamToOutput;
      try{
	tf_listener->waitForTransform(output_frame, msg->header.frame_id, msg->header.stamp, ros::Duration(1.0));
	tf_listener->lookupTransform(output_frame, msg->header.frame_id, msg->header.stamp, CamToOutput);
      }
      catch (tf::TransformException ex){
	ROS_ERROR("%s",ex.what());
      }

      //Init and clear visualization markers
      visualization_msgs::Marker rvizMarker;
      ar_track_alvar::AlvarMarker ar_pose_marker;
      arPoseMarkers_.markers.clear ();

      //Convert cloud to PCL 
      ARCloud cloud;
      pcl::fromROSMsg(*msg, cloud);

      //Get an OpenCV image from the cloud
      pcl::toROSMsg (cloud, *image_msg);
      image_msg->header.stamp = msg->header.stamp;
      image_msg->header.frame_id = msg->header.frame_id;
            
      //Convert the image
      capture_ = bridge_.imgMsgToCv (image_msg, "rgb8");

      //Get the estimated pose of the main markers by using all the markers in each bundle
      GetMultiMarkerPoses(capture_, cloud);

      for (size_t i=0; i<marker_detector.markers->size(); i++)
	{
	  int id = (*(marker_detector.markers))[i].GetId();	

	  // Draw if id is valid
	  if(id >= 0){

	    // Don't draw if it is a master tag...we do this later, a bit differently
	    bool should_draw = true;
	    for(int j=0; j<n_bundles; j++){
	      if(id == master_id[j]) should_draw = false;
	    }
	    if(should_draw){
	      Pose p = (*(marker_detector.markers))[i].pose;
	      makeMarkerMsgs(VISIBLE_MARKER, id, p, image_msg, CamToOutput, &rvizMarker, &ar_pose_marker);
	      rvizMarkerPub_.publish (rvizMarker);
	    }
	  }
	}
			
      //Draw the main markers, whether they are visible or not -- but only if at least 1 marker from their bundle is currently seen
      for(int i=0; i<n_bundles; i++)
	{
	  if(bundles_seen[i] == true){
	    makeMarkerMsgs(MAIN_MARKER, master_id[i], bundlePoses[i], image_msg, CamToOutput, &rvizMarker, &ar_pose_marker);
	    rvizMarkerPub_.publish (rvizMarker);
	    arPoseMarkers_.markers.push_back (ar_pose_marker);
	  }
	}

      //Publish the marker messages
      arMarkerPub_.publish (arPoseMarkers_);
    }
    catch (sensor_msgs::CvBridgeException & e){
      ROS_ERROR ("ar_track_alvar: Image error: %s", image_msg->encoding.c_str ());
    }
  }
}


int main(int argc, char *argv[])
{
  ros::init (argc, argv, "marker_detect");
  ros::NodeHandle n;

  if(argc < 8){
    std::cout << std::endl;
    cout << "Not enough arguments provided." << endl;
    cout << "Usage: ./findMarkerBundles <marker size in cm> <max new marker error> <max track error> <cam image topic> <cam info topic> <output frame> <list of bundle XML files...>" << endl;
    std::cout << std::endl;
    return 0;
  }

  // Get params from command line
  marker_size = atof(argv[1]);
  max_new_marker_error = atof(argv[2]);
  max_track_error = atof(argv[3]);
  cam_image_topic = argv[4];
  cam_info_topic = argv[5];
  output_frame = argv[6];
  int n_args_before_list = 7;
  n_bundles = argc - n_args_before_list;

  marker_detector.SetMarkerSize(marker_size);
  multi_marker_bundles = new MultiMarkerBundle*[n_bundles];	
  bundlePoses = new Pose[n_bundles];
  master_id = new int[n_bundles]; 
  bundle_indices = new std::vector<int>[n_bundles]; 
  bundles_seen = new bool[n_bundles]; 
  master_visible = new bool[n_bundles];
	
  median_n = 10;
  median_ind = new int[n_bundles];
  median_init = new bool[n_bundles];
  median_poses = new Pose*[n_bundles];
  for(int i=0; i<n_bundles; i++){
    median_init[i] = false;
    median_ind[i] = 0;
    median_poses[i] = new Pose[median_n];
  }

  // Load the marker bundle XML files
  for(int i=0; i<n_bundles; i++){	
    bundlePoses[i].Reset();		
    MultiMarker loadHelper;
    if(loadHelper.Load(argv[i + n_args_before_list], FILE_FORMAT_XML)){
      vector<int> id_vector = loadHelper.getIndices();
      multi_marker_bundles[i] = new MultiMarkerBundle(id_vector);	
      multi_marker_bundles[i]->Load(argv[i + n_args_before_list], FILE_FORMAT_XML);
      master_id[i] = multi_marker_bundles[i]->getMasterId();
      bundle_indices[i] = multi_marker_bundles[i]->getIndices();
    }
    else{
      cout<<"Cannot load file "<< argv[i + n_args_before_list] << endl;	
      return 0;
    }		
  }  

  // Set up camera, listeners, and broadcasters
  cam = new Camera(n, cam_info_topic);
  tf_listener = new tf::TransformListener(n);
  tf_broadcaster = new tf::TransformBroadcaster();
  arMarkerPub_ = n.advertise < ar_track_alvar::AlvarMarkers > ("ar_pose_marker", 0);
  rvizMarkerPub_ = n.advertise < visualization_msgs::Marker > ("visualization_marker", 0);
  rvizMarkerPub2_ = n.advertise < visualization_msgs::Marker > ("ARmarker_points", 0);
	
  //Give tf a chance to catch up before the camera callback starts asking for transforms
  ros::Duration(1.0).sleep();
  ros::spinOnce();			
	 
  //Subscribe to topics and set up callbacks
  ROS_INFO ("Subscribing to image topic");
  //image_transport::ImageTransport it_(n);
  //cam_sub_ = it_.subscribe (cam_image_topic, 1, &getCapCallback);
  cloud_sub_ = n.subscribe("/kinect_head/depth_registered/points", 1, &getPointCloudCallback);

  ros::spin();

  return 0;
}


