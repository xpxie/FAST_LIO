// This is an advanced implementation of the algorithm described in the
// following paper:
//   J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
//     Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

// Modifier: Livox               dev@livoxtech.com

// Copyright 2013, Ji Zhang, Carnegie Mellon University
// Further contributions copyright (c) 2016, Southwest Research Institute
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from this
//    software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
#include <omp.h>
#include <mutex>
#include <math.h>
#include <thread>
#include <fstream>
#include <csignal>
#include <unistd.h>
#include <Python.h>
#include <so3_math.h>
#include <ros/ros.h>
#include <message_filters/subscriber.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/synchronizer.h>
#include <Eigen/Core>
#include "IMU_Processing.hpp"
#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <visualization_msgs/Marker.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/io/pcd_io.h>
#include <sensor_msgs/PointCloud2.h>
#include <tf/transform_datatypes.h>
#include <tf/transform_broadcaster.h>
#include <geometry_msgs/Vector3.h>
#include <livox_ros_driver/CustomMsg.h>
#include "preprocess.h"
#include <ikd-Tree/ikd_Tree.h>

#define INIT_TIME           (0.1)
#define LASER_POINT_COV     (0.001)     //雷达点的噪声，可以引入模型
#define MAXN                (720000)
#define PUBFRAME_PERIOD     (20)

/*** Time Log Variables ***/
double kdtree_incremental_time = 0.0, kdtree_search_time = 0.0, kdtree_delete_time = 0.0;
double T1[MAXN], s_plot[MAXN], s_plot2[MAXN], s_plot3[MAXN], s_plot4[MAXN], s_plot5[MAXN], s_plot6[MAXN], s_plot7[MAXN], s_plot8[MAXN], s_plot9[MAXN], s_plot10[MAXN], s_plot11[MAXN];
double match_time = 0, solve_time = 0, solve_const_H_time = 0;
int    kdtree_size_st = 0, kdtree_size_end = 0, add_point_size = 0, kdtree_delete_counter = 0;
bool   runtime_pos_log = false, pcd_save_en = false, time_sync_en = false, extrinsic_est_en = true, path_en = true;
/**************************/

float res_last[100000] = {0.0};
float DET_RANGE = 300.0f;
const float MOV_THRESHOLD = 1.5f;

mutex mtx_buffer;
condition_variable sig_buffer;

string root_dir = ROOT_DIR;
string map_file_path, lid_topic, lid_r_topic, imu_topic, wheel_topic,altimeter_topic;

double res_mean_last = 0.05, total_residual = 0.0;
double last_timestamp_lidar = 0, r_last_timestamp_lidar = 0, last_timestamp_imu = -1.0, last_timestamp_wheel=0.0, last_timestamp_altimeter=0.0;
double gyr_cov = 0.1, acc_cov = 0.1, b_gyr_cov = 0.0001, b_acc_cov = 0.0001;
double filter_size_corner_min = 0, filter_size_surf_min = 0, filter_size_map_min = 0, fov_deg = 0;
double cube_len = 0, HALF_FOV_COS = 0, FOV_DEG = 0, total_distance = 0, lidar_end_time = 0, first_lidar_time = 0.0;
int    effct_feat_num = 0, time_log_counter = 0, scan_count = 0, r_scan_count = 0,publish_count, wheel_count = 0, altimeter_count = 0;
int    iterCount = 0, feats_down_size = 0, NUM_MAX_ITERATIONS = 0, laserCloudValidNum = 0, pcd_save_interval = -1, pcd_index = 0;
bool   point_selected_surf[100000] = {0};
bool   lidar_pushed, flg_first_scan = true, flg_exit = false, flg_EKF_inited;
bool   scan_pub_en = false, dense_pub_en = false, scan_body_pub_en = false;
bool wheel_update_flag=0;
double wheel_cov=1e-4,left_wheel_diameter=0.623479,right_wheel_diameter=0.622806,wheel_base=1.52439;

vector<vector<int>>  pointSearchInd_surf; 
vector<BoxPointType> cub_needrm;
vector<PointVector>  Nearest_Points;
vector<double>       extrinT_W(3, 0.0);
vector<double>       extrinR_W(9, 0.0);
vector<double>       extrinT(3, 0.0);
vector<double>       extrinR(9, 0.0);
vector<double>       r2l_extrinT(3, 0.0);
vector<double>       r2l_extrinR(9, 0.0);
deque<double>                     time_buffer;
deque<double>                     l_time_buffer;
deque<double>                     r_time_buffer;
deque<PointCloudXYZI::Ptr>        lidar_buffer;
deque<PointCloudXYZI::Ptr>        l_lidar_buffer;
deque<PointCloudXYZI::Ptr>        r_lidar_buffer;
bool l_lidar_update_flag=0;
deque<sensor_msgs::Imu::ConstPtr> imu_buffer;
deque<nav_msgs::Odometry::ConstPtr> wheel_buffer;
deque<fast_lio::altimeter::ConstPtr> altimeter_buffer;

PointCloudXYZI::Ptr featsFromMap(new PointCloudXYZI());
PointCloudXYZI::Ptr feats_undistort(new PointCloudXYZI());
PointCloudXYZI::Ptr feats_down_body(new PointCloudXYZI());
PointCloudXYZI::Ptr feats_down_world(new PointCloudXYZI());
PointCloudXYZI::Ptr normvec(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr laserCloudOri(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr corr_normvect(new PointCloudXYZI(100000, 1));
PointCloudXYZI::Ptr _featsArray;

PointCloudXYZI::Ptr  ptr_cor_1(new PointCloudXYZI());
PointCloudXYZI::Ptr  ptr_full_1(new PointCloudXYZI());

pcl::VoxelGrid<PointType> downSizeFilterSurf;
pcl::VoxelGrid<PointType> downSizeFilterMap;

KD_TREE<PointType> ikdtree;

V3F XAxisPoint_body(LIDAR_SP_LEN, 0.0, 0.0);
V3F XAxisPoint_world(LIDAR_SP_LEN, 0.0, 0.0);
V3D euler_cur;
V3D position_last(Zero3d);
V3D Lidar_T_wrt_IMU(Zero3d);
M3D Lidar_R_wrt_IMU(Eye3d);
V3D Wheel_T_wrt_IMU(Zero3d);
M3D Wheel_R_wrt_IMU(Eye3d);
V3D wheel_velocity_E;
V3D wheel_velocity_W;
V3D wheel_angvel_W;

/*** EKF inputs and output ***/
MeasureGroup Measures;
esekfom::esekf<state_ikfom, 12, input_ikfom> kf;
state_ikfom state_point;
vect3 pos_lid;

nav_msgs::Path path;
nav_msgs::Odometry odomAftMapped;
geometry_msgs::Quaternion geoQuat;
geometry_msgs::PoseStamped msg_body_pose;

shared_ptr<Preprocess> p_pre(new Preprocess());
shared_ptr<ImuProcess> p_imu(new ImuProcess());

void SigHandle(int sig)
{
    flg_exit = true;
    ROS_WARN("catch sig %d", sig);
    sig_buffer.notify_all();
}

inline void dump_lio_state_to_log(FILE *fp)  
{
    V3D rot_ang(Log(state_point.rot.toRotationMatrix()));
    fprintf(fp, "%lf ", Measures.lidar_beg_time - first_lidar_time);
    fprintf(fp, "%lf %lf %lf ", rot_ang(0), rot_ang(1), rot_ang(2));                   // Angle
    fprintf(fp, "%lf %lf %lf ", state_point.pos(0), state_point.pos(1), state_point.pos(2)); // Pos  
    fprintf(fp, "%lf %lf %lf ", 0.0, 0.0, 0.0);                                        // omega  
    fprintf(fp, "%lf %lf %lf ", state_point.vel(0), state_point.vel(1), state_point.vel(2)); // Vel  
    fprintf(fp, "%lf %lf %lf ", 0.0, 0.0, 0.0);                                        // Acc  
    fprintf(fp, "%lf %lf %lf ", state_point.bg(0), state_point.bg(1), state_point.bg(2));    // Bias_g  
    fprintf(fp, "%lf %lf %lf ", state_point.ba(0), state_point.ba(1), state_point.ba(2));    // Bias_a  
    fprintf(fp, "%lf %lf %lf ", state_point.grav[0], state_point.grav[1], state_point.grav[2]); // Bias_a  
    fprintf(fp, "\r\n");  
    fflush(fp);
}

void pointBodyToWorld_ikfom(PointType const * const pi, PointType * const po, state_ikfom &s)
{
    V3D p_body(pi->x, pi->y, pi->z);
    V3D p_global(s.rot * (s.offset_R_L_I*p_body + s.offset_T_L_I) + s.pos);

    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->intensity = pi->intensity;
}


void pointBodyToWorld(PointType const * const pi, PointType * const po)
{
    V3D p_body(pi->x, pi->y, pi->z);
    V3D p_global(state_point.rot * (state_point.offset_R_L_I*p_body + state_point.offset_T_L_I) + state_point.pos);

    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->intensity = pi->intensity;
}

template<typename T>
void pointBodyToWorld(const Matrix<T, 3, 1> &pi, Matrix<T, 3, 1> &po)
{
    V3D p_body(pi[0], pi[1], pi[2]);
    V3D p_global(state_point.rot * (state_point.offset_R_L_I*p_body + state_point.offset_T_L_I) + state_point.pos);//lidar-imu imu-world Rp+t

    po[0] = p_global(0);
    po[1] = p_global(1);
    po[2] = p_global(2);
}

void RGBpointBodyToWorld(PointType const * const pi, PointType * const po)
{
    V3D p_body(pi->x, pi->y, pi->z);
    V3D p_global(state_point.rot * (state_point.offset_R_L_I*p_body + state_point.offset_T_L_I) + state_point.pos);

    po->x = p_global(0);
    po->y = p_global(1);
    po->z = p_global(2);
    po->intensity = pi->intensity;
}

void RGBpointBodyLidarToIMU(PointType const * const pi, PointType * const po)
{
    V3D p_body_lidar(pi->x, pi->y, pi->z);
    V3D p_body_imu(state_point.offset_R_L_I*p_body_lidar + state_point.offset_T_L_I);

    po->x = p_body_imu(0);
    po->y = p_body_imu(1);
    po->z = p_body_imu(2);
    po->intensity = pi->intensity;
}

void points_cache_collect()
{
    PointVector points_history;
    ikdtree.acquire_removed_points(points_history);
    // for (int i = 0; i < points_history.size(); i++) _featsArray->push_back(points_history[i]);
}

BoxPointType LocalMap_Points;
bool Localmap_Initialized = false;
void lasermap_fov_segment()
{
    cub_needrm.clear();
    kdtree_delete_counter = 0;
    kdtree_delete_time = 0.0;    
    pointBodyToWorld(XAxisPoint_body, XAxisPoint_world);
    V3D pos_LiD = pos_lid;
    if (!Localmap_Initialized){
        for (int i = 0; i < 3; i++){
            LocalMap_Points.vertex_min[i] = pos_LiD(i) - cube_len / 2.0;
            LocalMap_Points.vertex_max[i] = pos_LiD(i) + cube_len / 2.0;
        }
        Localmap_Initialized = true;
        return;
    }
    float dist_to_map_edge[3][2];       //局部地图边界
    bool need_move = false;
    for (int i = 0; i < 3; i++){
        dist_to_map_edge[i][0] = fabs(pos_LiD(i) - LocalMap_Points.vertex_min[i]);
        dist_to_map_edge[i][1] = fabs(pos_LiD(i) - LocalMap_Points.vertex_max[i]);
        if (dist_to_map_edge[i][0] <= MOV_THRESHOLD * DET_RANGE || dist_to_map_edge[i][1] <= MOV_THRESHOLD * DET_RANGE) need_move = true;
    }
    if (!need_move) return;
    BoxPointType New_LocalMap_Points, tmp_boxpoints;
    New_LocalMap_Points = LocalMap_Points;
    float mov_dist = max((cube_len - 2.0 * MOV_THRESHOLD * DET_RANGE) * 0.5 * 0.9, double(DET_RANGE * (MOV_THRESHOLD -1)));
    for (int i = 0; i < 3; i++){
        tmp_boxpoints = LocalMap_Points;
        if (dist_to_map_edge[i][0] <= MOV_THRESHOLD * DET_RANGE){
            New_LocalMap_Points.vertex_max[i] -= mov_dist;
            New_LocalMap_Points.vertex_min[i] -= mov_dist;
            tmp_boxpoints.vertex_min[i] = LocalMap_Points.vertex_max[i] - mov_dist;
            cub_needrm.push_back(tmp_boxpoints);
        } else if (dist_to_map_edge[i][1] <= MOV_THRESHOLD * DET_RANGE){
            New_LocalMap_Points.vertex_max[i] += mov_dist;
            New_LocalMap_Points.vertex_min[i] += mov_dist;
            tmp_boxpoints.vertex_max[i] = LocalMap_Points.vertex_min[i] + mov_dist;
            cub_needrm.push_back(tmp_boxpoints);
        }
    }
    LocalMap_Points = New_LocalMap_Points;

    points_cache_collect();
    double delete_begin = omp_get_wtime();
    if(cub_needrm.size() > 0) kdtree_delete_counter = ikdtree.Delete_Point_Boxes(cub_needrm);
    kdtree_delete_time = omp_get_wtime() - delete_begin;
}

//void standard_pcl_cbk(const sensor_msgs::PointCloud2::ConstPtr &msg)
//{
//    mtx_buffer.lock();
//    scan_count ++;
//    double preprocess_start_time = omp_get_wtime();
//    if (msg->header.stamp.toSec() < last_timestamp_lidar)
//    {
//        ROS_ERROR("lidar loop back, clear buffer");
//        lidar_buffer.clear();
//    }
//
//    PointCloudXYZI::Ptr  ptr(new PointCloudXYZI());
//    p_pre->process(msg, ptr);
//    lidar_buffer.push_back(ptr);
//    time_buffer.push_back(msg->header.stamp.toSec()-0.1);
//    last_timestamp_lidar = msg->header.stamp.toSec();
//    s_plot11[scan_count] = omp_get_wtime() - preprocess_start_time;
//    mtx_buffer.unlock();
//    sig_buffer.notify_all();
//}

void standard_pcl_cbk(const sensor_msgs::PointCloud2::ConstPtr &msg)
{
    mtx_buffer.lock();
    double preprocess_start_time = omp_get_wtime();
    if (msg->header.stamp.toSec() < last_timestamp_lidar)
    {
        ROS_ERROR("lidar loop back, clear buffer");
        lidar_buffer.clear();
        l_lidar_buffer.clear();
    }

    PointCloudXYZI::Ptr  ptr(new PointCloudXYZI());
    p_pre->process(msg, ptr);


    l_lidar_buffer.push_back(ptr);
    l_time_buffer.push_back(msg->header.stamp.toSec()-0.1);
    last_timestamp_lidar = msg->header.stamp.toSec();
    s_plot11[scan_count] = omp_get_wtime() - preprocess_start_time;
    l_lidar_update_flag=1;
    mtx_buffer.unlock();
    sig_buffer.notify_all();
}

void standard_pcl_r_cbk(const sensor_msgs::PointCloud2::ConstPtr &msg)
{
    mtx_buffer.lock();

    PointCloudXYZI::Ptr  ptr2(new PointCloudXYZI());
    p_pre->process(msg, ptr2);

    if(l_lidar_update_flag&&!l_time_buffer.empty()&&!l_lidar_buffer.empty())
    {
        scan_count ++;
        double r_2_l_time_offset=msg->header.stamp.toSec()-l_time_buffer.back()-0.1;
        PointCloudXYZI::Ptr  ptr(new PointCloudXYZI());
        PointCloudXYZI::Ptr  ptr3(new PointCloudXYZI());
        PointCloudXYZI::Ptr  ptr_r_residual(new PointCloudXYZI());
        ptr=l_lidar_buffer.back();
        ptr3=ptr;

        if(!r_lidar_buffer.empty()&&!r_time_buffer.empty())
        {
            PointCloudXYZI::Ptr  ptr_r_last_residual(new PointCloudXYZI());
            ptr_r_last_residual=r_lidar_buffer.back();
            for(int pi=0;pi<ptr_r_last_residual->points.size();pi++)
            {
                PointType tmp_point=ptr_r_last_residual->points.at(pi);
                tmp_point.curvature-=100;
                ptr2->points.push_back(tmp_point);
            }
            r_lidar_buffer.pop_back();
            r_time_buffer.pop_back();
        }

        for(int pi=0;pi<ptr2->points.size();pi++)
        {
            if(ptr2->points.at(pi).curvature/1000+r_2_l_time_offset<=0.1&&ptr2->points.at(pi).curvature/1000+r_2_l_time_offset>0)
            {
                PointType tmp_point=ptr2->points.at(pi);
                tmp_point.x = r2l_extrinR[0]*ptr2->points.at(pi).x+r2l_extrinR[1]*ptr2->points.at(pi).y+r2l_extrinR[2]*ptr2->points.at(pi).z+r2l_extrinT[0];
                tmp_point.y = r2l_extrinR[3]*ptr2->points.at(pi).x+r2l_extrinR[4]*ptr2->points.at(pi).y+r2l_extrinR[5]*ptr2->points.at(pi).z+r2l_extrinT[1];
                tmp_point.z = r2l_extrinR[6]*ptr2->points.at(pi).x+r2l_extrinR[7]*ptr2->points.at(pi).y+r2l_extrinR[8]*ptr2->points.at(pi).z+r2l_extrinT[2];
                tmp_point.curvature+=r_2_l_time_offset;
                ptr3->points.push_back(tmp_point);
            }
            else
            {
                ptr_r_residual->points.push_back(ptr2->points.at(pi));
            }
        }
//        ROS_WARN("V1 Point is %zu",ptr3->points.size());
        lidar_buffer.push_back(ptr3);
        time_buffer.push_back(l_time_buffer.back());
        l_time_buffer.pop_back();
        l_lidar_buffer.pop_back();
        r_lidar_buffer.push_back(ptr_r_residual);
        r_time_buffer.push_back(msg->header.stamp.toSec()-0.1);
        l_lidar_update_flag=0;
    }

    mtx_buffer.unlock();
    sig_buffer.notify_all();
}

void standard_pcl2_cbk(const sensor_msgs::PointCloud2::ConstPtr &msg,const sensor_msgs::PointCloud2::ConstPtr &msg2)
{
    mtx_buffer.lock();
    scan_count ++;
    double preprocess_start_time = omp_get_wtime();
    double r_wrt_l_time_offset=msg2->header.stamp.toSec()-msg->header.stamp.toSec();
//    ROS_WARN("L1 time is :%f,L2 time is :%f",msg->header.stamp.toSec(),msg2->header.stamp.toSec());
    //left lidar buffer
    if (msg->header.stamp.toSec() < last_timestamp_lidar && msg2->header.stamp.toSec() < last_timestamp_lidar)
    {
        ROS_ERROR("lidar loop back, clear buffer");
        lidar_buffer.clear();
    }

    PointCloudXYZI::Ptr  ptr(new PointCloudXYZI());

    p_pre->process(msg, ptr);
    for(int i=0; i<p_pre->pl_corn.size();i++)
    {
        ptr_cor_1->push_back(p_pre->pl_corn.points[i]);
    }
    for(int i=0; i<p_pre->pl_full.size();i++)
    {
        ptr_full_1->push_back(p_pre->pl_full.points[i]);
    }

    PointCloudXYZI::Ptr  ptr2(new PointCloudXYZI());
    p_pre->process(msg2, ptr2);
    PointCloudXYZI::Ptr  ptr3(new PointCloudXYZI());
    ptr3=ptr;

//    ROS_WARN("V1 Point is %zu",ptr3->points.size());
    for(int pi=0;pi<ptr2->points.size();pi++)
    {
        PointType tmp_point=ptr2->points.at(pi);
        tmp_point.x = r2l_extrinR[0]*ptr2->points.at(pi).x+r2l_extrinR[1]*ptr2->points.at(pi).y+r2l_extrinR[2]*ptr2->points.at(pi).z+r2l_extrinT[0];
        tmp_point.y = r2l_extrinR[3]*ptr2->points.at(pi).x+r2l_extrinR[4]*ptr2->points.at(pi).y+r2l_extrinR[5]*ptr2->points.at(pi).z+r2l_extrinT[1];
        tmp_point.z = r2l_extrinR[6]*ptr2->points.at(pi).x+r2l_extrinR[7]*ptr2->points.at(pi).y+r2l_extrinR[8]*ptr2->points.at(pi).z+r2l_extrinT[2];
        tmp_point.curvature+=r_wrt_l_time_offset;
        ptr3->points.push_back(tmp_point);
    }
//    ROS_WARN("V1+V2 Point is %zu",ptr3->points.size());

    lidar_buffer.push_back(ptr3);
    if(msg->header.stamp.toSec() > msg2->header.stamp.toSec())
    {
//        time_buffer.push_back(msg->header.stamp.toSec()-ptr->points.end()->curvature/double(1000));
//        last_timestamp_lidar = msg->header.stamp.toSec()-ptr->points.end()->curvature/double(1000);
        time_buffer.push_back(msg->header.stamp.toSec());
        last_timestamp_lidar = msg->header.stamp.toSec();
    }
    else
    {
        time_buffer.push_back(msg2->header.stamp.toSec());
        last_timestamp_lidar = msg2->header.stamp.toSec();
    }

    s_plot11[scan_count] = omp_get_wtime() - preprocess_start_time;
    mtx_buffer.unlock();
    sig_buffer.notify_all();
}

double timediff_lidar_wrt_imu = 0.0;
bool   timediff_set_flg = false;
void livox_pcl_cbk(const livox_ros_driver::CustomMsg::ConstPtr &msg) 
{
    mtx_buffer.lock();
    double preprocess_start_time = omp_get_wtime();
    scan_count ++;
    if (msg->header.stamp.toSec() < last_timestamp_lidar)
    {
        ROS_ERROR("lidar loop back, clear buffer");
        lidar_buffer.clear();
    }
    last_timestamp_lidar = msg->header.stamp.toSec();
    
    if (!time_sync_en && abs(last_timestamp_imu - last_timestamp_lidar) > 10.0 && !imu_buffer.empty() && !lidar_buffer.empty() )
    {
        printf("IMU and LiDAR not Synced, IMU time: %lf, lidar header time: %lf \n",last_timestamp_imu, last_timestamp_lidar);
    }

    if (time_sync_en && !timediff_set_flg && abs(last_timestamp_lidar - last_timestamp_imu) > 1 && !imu_buffer.empty())
    {
        timediff_set_flg = true;
        timediff_lidar_wrt_imu = last_timestamp_lidar + 0.1 - last_timestamp_imu;
        printf("Self sync IMU and LiDAR, time diff is %.10lf \n", timediff_lidar_wrt_imu);
    }

    PointCloudXYZI::Ptr  ptr(new PointCloudXYZI());
    p_pre->process(msg, ptr);
    lidar_buffer.push_back(ptr);
    time_buffer.push_back(last_timestamp_lidar);
    
    s_plot11[scan_count] = omp_get_wtime() - preprocess_start_time;
    mtx_buffer.unlock();
    sig_buffer.notify_all();
}

void imu_cbk(const sensor_msgs::Imu::ConstPtr &msg_in) 
{
    publish_count ++;
    // cout<<"IMU got at: "<<msg_in->header.stamp.toSec()<<endl;
    sensor_msgs::Imu::Ptr msg(new sensor_msgs::Imu(*msg_in));

    if (abs(timediff_lidar_wrt_imu) > 0.1 && time_sync_en)
    {
        msg->header.stamp = \
        ros::Time().fromSec(timediff_lidar_wrt_imu + msg_in->header.stamp.toSec());
    }

    double timestamp = msg->header.stamp.toSec();

    mtx_buffer.lock();

    if (timestamp < last_timestamp_imu)
    {
        ROS_WARN("imu loop back, clear buffer");
        imu_buffer.clear();
    }

    last_timestamp_imu = timestamp;

    imu_buffer.push_back(msg);
    mtx_buffer.unlock();
    sig_buffer.notify_all();
}

void wheel_cbk(const nav_msgs::Odometry::ConstPtr &msg_in)
{
    wheel_count++;

//    ROS_WARN("wheel data count:%d",wheel_count);

    nav_msgs::Odometry::Ptr msg(new nav_msgs::Odometry(*msg_in));

    double timestamp = msg->header.stamp.toSec();

    mtx_buffer.lock();

    if (timestamp < last_timestamp_wheel)
    {
        ROS_WARN("wheel loop back, clear buffer");
        wheel_buffer.clear();
    }

    last_timestamp_wheel = timestamp;

    wheel_buffer.push_back(msg);
    mtx_buffer.unlock();
    sig_buffer.notify_all();
}

//void altimeter_cbk(const fast_lio::altimeter::ConstPtr &msg_in)
//{
//    altimeter_count++;
//
////    ROS_WARN("wheel data count:%d",wheel_count);
//
//    fast_lio::altimeter::Ptr msg(new fast_lio::altimeter(*msg_in));
//
//    double timestamp = msg->header.stamp.toSec();
//
//    mtx_buffer.lock();
//
//    if (timestamp < last_timestamp_altimeter)
//    {
//        ROS_WARN("altimeter loop back, clear buffer");
//        altimeter_buffer.clear();
//    }
//
//    last_timestamp_altimeter = timestamp;
//
//    altimeter_buffer.push_back(msg);
//    mtx_buffer.unlock();
//    sig_buffer.notify_all();
//}


double lidar_mean_scantime = 0.0;
int    scan_num = 0;
bool sync_packages(MeasureGroup &meas)
{
    if (lidar_buffer.empty() || imu_buffer.empty()) {

        return false;
    }

    /*** push a lidar scan ***/
    if(!lidar_pushed)
    {
        meas.lidar = lidar_buffer.front();
        meas.lidar_beg_time = time_buffer.front();
        // time too little
        if (meas.lidar->points.size() <= 1 )
        {
            lidar_end_time = meas.lidar_beg_time + lidar_mean_scantime;
            ROS_WARN("Too few input point cloud!\n");
        }
        else if (meas.lidar->points.back().curvature / double(1000) < 0.5 * lidar_mean_scantime)
        {
            lidar_end_time = meas.lidar_beg_time + lidar_mean_scantime;
        }
        else
        {
            scan_num ++;
            lidar_end_time = meas.lidar_beg_time + meas.lidar->points.back().curvature / double(1000);
            lidar_mean_scantime += (meas.lidar->points.back().curvature / double(1000) - lidar_mean_scantime) / scan_num;
        }

        meas.lidar_end_time = lidar_end_time;
//        meas.lidar_end_time = time_buffer.front();
//        // time too little
//        if (meas.lidar->points.size() <= 1 )
//        {
//            meas.lidar_beg_time = meas.lidar_end_time - lidar_mean_scantime;
//            ROS_WARN("Too few input point cloud!\n");
//        }
//        else if (meas.lidar->points.back().curvature / double(1000) < 0.5 * lidar_mean_scantime)
//        {
//            meas.lidar_beg_time = meas.lidar_end_time - lidar_mean_scantime;
//        }
//        else
//        {
//            scan_num ++;
//            meas.lidar_beg_time = meas.lidar_end_time - meas.lidar->points.back().curvature / double(1000);
//            lidar_mean_scantime += (meas.lidar->points.back().curvature / double(1000) - lidar_mean_scantime) / scan_num;
//        }
//
//       lidar_end_time =  meas.lidar_end_time;

        lidar_pushed = true;
    }

    if (last_timestamp_imu < lidar_end_time )
    {
        return false;
    }

    /*** push imu data, and pop from imu buffer ***/
    double imu_time = imu_buffer.front()->header.stamp.toSec();
    meas.imu.clear();
    while ((!imu_buffer.empty()) &&(imu_time < lidar_end_time))
    {
        imu_time = imu_buffer.front()->header.stamp.toSec();
        if(imu_time > lidar_end_time) break;
        meas.imu.push_back(imu_buffer.front());
        imu_buffer.pop_front();
    }

//    double wheel_time = wheel_buffer.front()->header.stamp.toSec();
//    meas.wheel.clear();
//    while ((!wheel_buffer.empty())&&(wheel_time < lidar_end_time))
//    {
//        wheel_time = wheel_buffer.front()->header.stamp.toSec();
//        if(wheel_time > lidar_end_time) break;
//        meas.wheel.push_back(wheel_buffer.front());
//        wheel_buffer.pop_front();
//    }

//    double altimeter_time = altimeter_buffer.front()->header.stamp.toSec();
//    meas.altimeter.clear();
//    while ((!altimeter_buffer.empty())&&(altimeter_time < lidar_end_time))
//    {
//        altimeter_time = altimeter_buffer.front()->header.stamp.toSec();
//        if(altimeter_time > lidar_end_time) break;
//        meas.altimeter.push_back(altimeter_buffer.front());
//        altimeter_buffer.pop_front();
//    }

    lidar_buffer.pop_front();
    time_buffer.pop_front();
    lidar_pushed = false;
    return true;
}

int process_increments = 0;
void map_incremental()
{
    PointVector PointToAdd;
    PointVector PointNoNeedDownsample;
    PointToAdd.reserve(feats_down_size);
    PointNoNeedDownsample.reserve(feats_down_size);
    for (int i = 0; i < feats_down_size; i++)
    {
        /* transform to world frame */
        pointBodyToWorld(&(feats_down_body->points[i]), &(feats_down_world->points[i]));
        /* decide if need add to map */
        if (!Nearest_Points[i].empty() && flg_EKF_inited)
        {
            const PointVector &points_near = Nearest_Points[i];
            bool need_add = true;
            BoxPointType Box_of_Point;
            PointType downsample_result, mid_point;
            //所属方块的中心
            mid_point.x = floor(feats_down_world->points[i].x/filter_size_map_min)*filter_size_map_min + 0.5 * filter_size_map_min;
            mid_point.y = floor(feats_down_world->points[i].y/filter_size_map_min)*filter_size_map_min + 0.5 * filter_size_map_min;
            mid_point.z = floor(feats_down_world->points[i].z/filter_size_map_min)*filter_size_map_min + 0.5 * filter_size_map_min;
            float dist  = calc_dist(feats_down_world->points[i],mid_point);
            if (fabs(points_near[0].x - mid_point.x) > 0.5 * filter_size_map_min && fabs(points_near[0].y - mid_point.y) > 0.5 * filter_size_map_min && fabs(points_near[0].z - mid_point.z) > 0.5 * filter_size_map_min){
                PointNoNeedDownsample.push_back(feats_down_world->points[i]);
                continue;
            }
            for (int readd_i = 0; readd_i < NUM_MATCH_POINTS; readd_i ++)
            {
                if (points_near.size() < NUM_MATCH_POINTS) break;
                if (calc_dist(points_near[readd_i], mid_point) < dist)
                {
                    need_add = false;
                    break;
                }
            }
            if (need_add) PointToAdd.push_back(feats_down_world->points[i]);
        }
        else
        {
            PointToAdd.push_back(feats_down_world->points[i]);
        }
    }

    double st_time = omp_get_wtime();
    add_point_size = ikdtree.Add_Points(PointToAdd, true);
    ikdtree.Add_Points(PointNoNeedDownsample, false); 
    add_point_size = PointToAdd.size() + PointNoNeedDownsample.size();
    kdtree_incremental_time = omp_get_wtime() - st_time;
}

PointCloudXYZI::Ptr pcl_wait_pub(new PointCloudXYZI(500000, 1));
PointCloudXYZI::Ptr pcl_wait_save(new PointCloudXYZI());
void publish_frame_world(const ros::Publisher & pubLaserCloudFull,const ros::Publisher & pubLaserCor,const ros::Publisher & pubLaserFull )
{
    if(scan_pub_en)
    {
        PointCloudXYZI::Ptr laserCloudFullRes(dense_pub_en ? feats_undistort : feats_down_body);
        int size = laserCloudFullRes->points.size();
        PointCloudXYZI::Ptr laserCloudWorld( \
                        new PointCloudXYZI(size, 1));

        for (int i = 0; i < size; i++)
        {
            RGBpointBodyToWorld(&laserCloudFullRes->points[i], \
                                &laserCloudWorld->points[i]);
//            laserCloudWorld->points[i].intensity=255;
        }

        sensor_msgs::PointCloud2 laserCloudmsg;
        pcl::toROSMsg(*laserCloudWorld, laserCloudmsg);
        laserCloudmsg.header.stamp = ros::Time().fromSec(lidar_end_time);
        laserCloudmsg.header.frame_id = "camera_init";
        pubLaserCloudFull.publish(laserCloudmsg);
        publish_count -= PUBFRAME_PERIOD;

//        int size_corn = p_pre->pl_corn.points.size();
//        PointCloudXYZI::Ptr cornCloudWorld( \
//                        new PointCloudXYZI(size_corn+ptr_cor_1->size(), 1));
//
//        for (int i = 0; i < size_corn; i++)
//        {
//            PointType tmp_point=p_pre->pl_corn.points[i];
//            tmp_point.x = r2l_extrinR[0]*p_pre->pl_corn.points[i].x+r2l_extrinR[1]*p_pre->pl_corn.points[i].y+r2l_extrinR[2]*p_pre->pl_corn.points[i].z+r2l_extrinT[0];
//            tmp_point.y = r2l_extrinR[3]*p_pre->pl_corn.points[i].x+r2l_extrinR[4]*p_pre->pl_corn.points[i].y+r2l_extrinR[5]*p_pre->pl_corn.points[i].z+r2l_extrinT[1];
//            tmp_point.z = r2l_extrinR[6]*p_pre->pl_corn.points[i].x+r2l_extrinR[7]*p_pre->pl_corn.points[i].y+r2l_extrinR[8]*p_pre->pl_corn.points[i].z+r2l_extrinT[2];
//
//            RGBpointBodyToWorld(&tmp_point, \
//                                &cornCloudWorld->points[i]);
//            cornCloudWorld->points[i].intensity=128;
//        }
//        for(int i =0;i<ptr_cor_1->size();i++)
//        {
//            RGBpointBodyToWorld(&ptr_cor_1->points[i], \
//                                &cornCloudWorld->points[size_corn+i]);
//            cornCloudWorld->points[size_corn+i].intensity=128;
//        }
//        ptr_cor_1->clear();
//
//        pcl::toROSMsg(*cornCloudWorld, laserCloudmsg);
//        laserCloudmsg.header.stamp = ros::Time().fromSec(lidar_end_time);
//        laserCloudmsg.header.frame_id = "camera_init";
//        pubLaserCor.publish(laserCloudmsg);
//
//
//
//        int size_full = p_pre->pl_full.points.size();
//        PointCloudXYZI::Ptr fullCloudWorld( \
//                        new PointCloudXYZI(size_full+ptr_full_1->size(), 1));
//
//        for (int i = 0; i < size_full; i++)
//        {
//
//            PointType tmp_point=p_pre->pl_full.points[i];
//            tmp_point.x = r2l_extrinR[0]*p_pre->pl_full.points[i].x+r2l_extrinR[1]*p_pre->pl_full.points[i].y+r2l_extrinR[2]*p_pre->pl_full.points[i].z+r2l_extrinT[0];
//            tmp_point.y = r2l_extrinR[3]*p_pre->pl_full.points[i].x+r2l_extrinR[4]*p_pre->pl_full.points[i].y+r2l_extrinR[5]*p_pre->pl_full.points[i].z+r2l_extrinT[1];
//            tmp_point.z = r2l_extrinR[6]*p_pre->pl_full.points[i].x+r2l_extrinR[7]*p_pre->pl_full.points[i].y+r2l_extrinR[8]*p_pre->pl_full.points[i].z+r2l_extrinT[2];
//
//            RGBpointBodyToWorld(&tmp_point, \
//                                &fullCloudWorld->points[i]);
//            fullCloudWorld->points[i].intensity=0;
//        }
//        for(int i =0;i<ptr_full_1->size();i++)
//        {
//            RGBpointBodyToWorld(&ptr_full_1->points[i], \
//                                &fullCloudWorld->points[size_full+i]);
//            fullCloudWorld->points[size_full+i].intensity=0;
//        }
//        ptr_full_1->clear();
//
//        pcl::toROSMsg(*fullCloudWorld, laserCloudmsg);
//        laserCloudmsg.header.stamp = ros::Time().fromSec(lidar_end_time);
//        laserCloudmsg.header.frame_id = "camera_init";
//        pubLaserFull.publish(laserCloudmsg);
    }

    /**************** save map ****************/
    /* 1. make sure you have enough memories
    /* 2. noted that pcd save will influence the real-time performences **/
    if (pcd_save_en)
    {
        int size = feats_undistort->points.size();
        PointCloudXYZI::Ptr laserCloudWorld( \
                        new PointCloudXYZI(size, 1));

        for (int i = 0; i < size; i++)
        {
            RGBpointBodyToWorld(&feats_undistort->points[i], \
                                &laserCloudWorld->points[i]);
        }
        *pcl_wait_save += *laserCloudWorld;

        static int scan_wait_num = 0;
        scan_wait_num ++;
        if (pcl_wait_save->size() > 0 && pcd_save_interval > 0  && scan_wait_num >= pcd_save_interval)
        {
            pcd_index ++;
            string all_points_dir(string(string(ROOT_DIR) + "PCD/scans_") + to_string(pcd_index) + string(".pcd"));
            pcl::PCDWriter pcd_writer;
            cout << "current scan saved to /PCD/" << all_points_dir << endl;
            pcd_writer.writeBinary(all_points_dir, *pcl_wait_save);
            pcl_wait_save->clear();
            scan_wait_num = 0;
        }
    }
}

void publish_frame_body(const ros::Publisher & pubLaserCloudFull_body)
{
    int size = feats_undistort->points.size();
    PointCloudXYZI::Ptr laserCloudIMUBody(new PointCloudXYZI(size, 1));

    for (int i = 0; i < size; i++)
    {
        RGBpointBodyLidarToIMU(&feats_undistort->points[i], \
                            &laserCloudIMUBody->points[i]);
    }

    sensor_msgs::PointCloud2 laserCloudmsg;
    pcl::toROSMsg(*laserCloudIMUBody, laserCloudmsg);
    laserCloudmsg.header.stamp = ros::Time().fromSec(lidar_end_time);
    laserCloudmsg.header.frame_id = "body";
    pubLaserCloudFull_body.publish(laserCloudmsg);
    publish_count -= PUBFRAME_PERIOD;
}

void publish_effect_world(const ros::Publisher & pubLaserCloudEffect)
{
    PointCloudXYZI::Ptr laserCloudWorld( \
                    new PointCloudXYZI(effct_feat_num, 1));
    for (int i = 0; i < effct_feat_num; i++)
    {
        RGBpointBodyToWorld(&laserCloudOri->points[i], \
                            &laserCloudWorld->points[i]);
    }
    sensor_msgs::PointCloud2 laserCloudFullRes3;
    pcl::toROSMsg(*laserCloudWorld, laserCloudFullRes3);
    laserCloudFullRes3.header.stamp = ros::Time().fromSec(lidar_end_time);
    laserCloudFullRes3.header.frame_id = "camera_init";
    pubLaserCloudEffect.publish(laserCloudFullRes3);
}

void publish_map(const ros::Publisher & pubLaserCloudMap)
{
    sensor_msgs::PointCloud2 laserCloudMap;
    pcl::toROSMsg(*featsFromMap, laserCloudMap);
    laserCloudMap.header.stamp = ros::Time().fromSec(lidar_end_time);
    laserCloudMap.header.frame_id = "camera_init";
    pubLaserCloudMap.publish(laserCloudMap);
}

template<typename T>
void set_posestamp(T & out)
{
    out.pose.position.x = state_point.pos(0);
    out.pose.position.y = state_point.pos(1);
    out.pose.position.z = state_point.pos(2);
    out.pose.orientation.x = geoQuat.x;
    out.pose.orientation.y = geoQuat.y;
    out.pose.orientation.z = geoQuat.z;
    out.pose.orientation.w = geoQuat.w;
    
}

M3D  extrinsic_I2G_R = M3D::Zero();
V3D extrinsic_I2G_T;
Isometry3d T_global=Isometry3d::Identity();;
bool gt_init=0;

void publish_odometry(const ros::Publisher & pubOdomAftMapped, FILE *fp )
{
    odomAftMapped.header.frame_id = "camera_init";
    odomAftMapped.child_frame_id = "body";
    odomAftMapped.header.stamp = ros::Time().fromSec(lidar_end_time);// ros::Time().fromSec(lidar_end_time);
    set_posestamp(odomAftMapped.pose);
    odomAftMapped.twist.twist.linear.x=state_point.vel.x();
    odomAftMapped.twist.twist.linear.y=state_point.vel.y();
    odomAftMapped.twist.twist.linear.z=state_point.vel.z();
    odomAftMapped.twist.twist.angular.x= wheel_velocity_E[0];
    odomAftMapped.twist.twist.angular.y= wheel_velocity_E[1];
    odomAftMapped.twist.twist.angular.z= wheel_velocity_E[2];
    pubOdomAftMapped.publish(odomAftMapped);




//    tf::Quaternion map_quat;
//    tf::quaternionMsgToTF(odomAftMapped.pose.pose.orientation, mevoap_quat);
//    double map_roll, map_pitch, map_yaw;
//    tf::Matrix3x3(map_quat).getRPY(map_roll, map_pitch, map_yaw);
//    ROS_WARN("Map odom YAW:%f",map_yaw);

    auto P = kf.get_P();
    for (int i = 0; i < 6; i ++)
    {
        int k = i < 3 ? i + 3 : i - 3;
        odomAftMapped.pose.covariance[i*6 + 0] = P(k, 3);
        odomAftMapped.pose.covariance[i*6 + 1] = P(k, 4);
        odomAftMapped.pose.covariance[i*6 + 2] = P(k, 5);
        odomAftMapped.pose.covariance[i*6 + 3] = P(k, 0);
        odomAftMapped.pose.covariance[i*6 + 4] = P(k, 1);
        odomAftMapped.pose.covariance[i*6 + 5] = P(k, 2);
    }

    static tf::TransformBroadcaster br;
    tf::Transform                   transform;
    tf::Quaternion                  q;
    transform.setOrigin(tf::Vector3(odomAftMapped.pose.pose.position.x, \
                                    odomAftMapped.pose.pose.position.y, \
                                    odomAftMapped.pose.pose.position.z));
    q.setW(odomAftMapped.pose.pose.orientation.w);
    q.setX(odomAftMapped.pose.pose.orientation.x);
    q.setY(odomAftMapped.pose.pose.orientation.y);
    q.setZ(odomAftMapped.pose.pose.orientation.z);
    transform.setRotation( q );
    br.sendTransform( tf::StampedTransform( transform, odomAftMapped.header.stamp, "camera_init", "body" ) );
//


    Eigen::Isometry3d T_temp=Eigen::Isometry3d::Identity();
    Eigen::Quaterniond eigen_q;
    Eigen::Vector3d temp_t;
    eigen_q.x()=q.x();
    eigen_q.y()=q.y();
    eigen_q.z()=q.z();
    eigen_q.w()=q.w();
    temp_t[0]=odomAftMapped.pose.pose.position.x;
    temp_t[1]=odomAftMapped.pose.pose.position.y;
    temp_t[2]=odomAftMapped.pose.pose.position.z;

    T_temp.rotate(eigen_q.toRotationMatrix());
    T_temp.pretranslate(temp_t);


    //for urban26
    if(lidar_end_time>1544580719.871842 && gt_init==0)
    {
        extrinsic_I2G_R << -0.9997840965,-0.0193252349,0.007635158537,0.01942543815,-0.9997231711,0.01327529918,0.007376496629,0.0134207493,0.9998827285;
        extrinsic_I2G_T << 330089.674400,4118151.702000,18.037571;
        T_global.rotate(extrinsic_I2G_R);
        T_global.pretranslate(extrinsic_I2G_T);
        T_global=T_global*T_temp.inverse();
        gt_init=1;
    }
    //for urban27
//    if(lidar_end_time>1544582651.831926 && gt_init==0)
//    {
//        extrinsic_I2G_R << -0.4175365857, -0.9083368378, 0.0242361024,0.9085893889, -0.4176883256, -0.001336089993,0.01133675679, 0.02146279901, 0.9997053697;
//        extrinsic_I2G_T << 328626.9008,4118415.219,17.9944406;
//        T_global.rotate(extrinsic_I2G_R);
//        T_global.pretranslate(extrinsic_I2G_T);
//        T_global=T_global*T_temp.inverse();
//        gt_init=1;
//    }
    if (gt_init==1)
    {
        T_temp=T_global*T_temp;
        Eigen::Quaterniond out_q= Eigen::Quaterniond(T_temp.rotation());
        fprintf(fp, "%lf %lf %lf %lf %lf %lf %lf %lf\n", lidar_end_time,T_temp(0,3),\
                    T_temp(1,3),T_temp(2,3),out_q.x(),out_q.y(),out_q.z(),out_q.w());
        fflush(fp);
    }

}

void publish_path(const ros::Publisher pubPath)
{
    set_posestamp(msg_body_pose);
    msg_body_pose.header.stamp = ros::Time().fromSec(lidar_end_time);
    msg_body_pose.header.frame_id = "camera_init";

    /*** if path is too large, the rvis will crash ***/
    static int jjj = 0;
    jjj++;
    if (jjj % 10 == 0)//1HZ
    {
        path.poses.push_back(msg_body_pose);
        pubPath.publish(path);
    }
}


void h_share_model(state_ikfom &s, esekfom::dyn_share_datastruct<double> &ekfom_data)
{
    if(wheel_update_flag)
    {
        ROS_WARN(" update wheel msg");
        ekfom_data.h_x = MatrixXd::Zero(4, 27);
        ekfom_data.R = (1/wheel_cov)*MatrixXd::Identity(4, 4);
        ekfom_data.h.resize(4);

//        const PointType &laser_p  = laserCloudOri->points[i];
//        V3D point_this_be(laser_p.x, laser_p.y, laser_p.z);
//        M3D point_be_crossmat;
//        point_be_crossmat << SKEW_SYM_MATRX(point_this_be);
//        V3D point_this = s.offset_R_L_I * point_this_be + s.offset_T_L_I;
//        M3D point_crossmat;
//        point_crossmat<<SKEW_SYM_MATRX(point_this);
//
//        /*** get the normal vector of closest surface/corner ***/
//        const PointType &norm_p = corr_normvect->points[i];
//        V3D norm_vec(norm_p.x, norm_p.y, norm_p.z);
//
//        /*** calculate the Measuremnt Jacobian matrix H ***/
//        V3D C(s.rot.conjugate() *norm_vec);
//        V3D A(point_crossmat * C);
//        V3D B(point_be_crossmat * s.offset_R_L_I.conjugate() * C);

        //origin model
        V3D wheel_velocity_I = s.offset_R_W_I * wheel_velocity_W;
        M3D wheel_vel_I_crossmat;
        wheel_vel_I_crossmat << SKEW_SYM_MATRX(wheel_velocity_I);
        M3D wheel_vel_W_crossmat;
        wheel_vel_W_crossmat << SKEW_SYM_MATRX(wheel_velocity_W);
        V3D wheel_angvel_I=s.offset_R_W_I * wheel_angvel_W;
        M3D wheel_angvel_W_crossmat;
        wheel_angvel_W_crossmat<< SKEW_SYM_MATRX(wheel_angvel_W);
        wheel_velocity_E = s.rot * s.offset_R_W_I * wheel_velocity_W;

        //vel contrain
        ekfom_data.h_x.block<3, 3>(0,3) << (wheel_vel_I_crossmat* s.rot.conjugate());
        ekfom_data.h_x.block<3, 3>(0,12) << 1*MatrixXd::Identity(3, 3);
        ekfom_data.h_x.block<3, 3>(0,24) << ( wheel_vel_W_crossmat * s.offset_R_W_I.conjugate()*s.rot.conjugate());
        // rpy contrain
//        ekfom_data.h_x.block<3, 3>(3,15) << -1*MatrixXd::Identity(3, 3);
//        ekfom_data.h_x.block<3, 3>(3,24) << ( wheel_angvel_W_crossmat* s.offset_R_W_I.conjugate());
//        ekfom_data.h(0)= 1*(wheel_velocity_E[0]-s.vel.x());
//        ekfom_data.h(1)= 1*(wheel_velocity_E[1]-s.vel.y());
//        ekfom_data.h(2)= 1*(wheel_velocity_E[2]-s.vel.z());
//        ekfom_data.h(3)= 1*(wheel_angvel_I[0]-(Measures.imu.back()->angular_velocity.x-s.bg(0)));
//        ekfom_data.h(4)= 1*(wheel_angvel_I[1]-(Measures.imu.back()->angular_velocity.y-s.bg(1)));
//        ekfom_data.h(5)= 1*(wheel_angvel_I[2]-(Measures.imu.back()->angular_velocity.z-s.bg(2)));
        // yaw constrain
        ekfom_data.h_x.block<1, 3>(3,15) << 0,0,-1;
        ekfom_data.h_x.block<1, 3>(3,24) << ((wheel_angvel_W_crossmat* s.offset_R_W_I.conjugate()).block<1,3>(2,0));
        ekfom_data.h(0)= 1*(wheel_velocity_E[0]-s.vel.x());
        ekfom_data.h(1)= 1*(wheel_velocity_E[1]-s.vel.y());
        ekfom_data.h(2)= 1*(wheel_velocity_E[2]-s.vel.z());
        ekfom_data.h(3)= 1*(wheel_angvel_I[2]-(Measures.imu.back()->angular_velocity.z-s.bg(2)));

//      rigid model
//        M3D wheel_translation_I_crossmat;
//        wheel_translation_I_crossmat << SKEW_SYM_MATRX(s.offset_T_W_I);
//        M3D pos_crossmat;
//        pos_crossmat << SKEW_SYM_MATRX(s.pos);
//        V3D gyro_unbiasd;
//        gyro_unbiasd << (Measures.imu.back()->angular_velocity.x-s.bg(0)), (Measures.imu.back()->angular_velocity.y-s.bg(1)),
//                        (Measures.imu.back()->angular_velocity.z-s.bg(2));
//        M3D gyro_crossmat_I;
//        gyro_crossmat_I << SKEW_SYM_MATRX(gyro_unbiasd);
//        M3D gyro_crossmat_E;
//        gyro_crossmat_E << SKEW_SYM_MATRX((s.rot * gyro_unbiasd));
//        V3D wheel_velocity_I = wheel_translation_I_crossmat * gyro_unbiasd + s.offset_R_W_I * wheel_velocity_W;
//        M3D wheel_vel_I_crossmat;
//        wheel_vel_I_crossmat << SKEW_SYM_MATRX(wheel_velocity_I);
//        M3D wheel_vel_W_crossmat;
//        wheel_vel_W_crossmat << SKEW_SYM_MATRX(wheel_velocity_W);
//        wheel_velocity_E = pos_crossmat * s.rot * gyro_unbiasd + s.rot  * wheel_velocity_I;
//
//        ekfom_data.h_x.block<3, 3>(0,0) << -1 * gyro_crossmat_E;
//        ekfom_data.h_x.block<3, 3>(0,3) << ((wheel_vel_I_crossmat* s.rot.conjugate()) + (gyro_crossmat_I*s.rot.conjugate() * pos_crossmat));
//        ekfom_data.h_x.block<3, 3>(0,12) << 1*MatrixXd::Identity(3, 3);
//        ekfom_data.h_x.block<3, 3>(0,21) << gyro_crossmat_I * s.rot;
//        ekfom_data.h_x.block<3, 3>(0,24) << (wheel_vel_W_crossmat * s.offset_R_W_I.conjugate()*s.rot.conjugate());





    ROS_WARN("residul is %f",ekfom_data.h(0));
//        ROS_WARN("angvel residul is %f",ekfom_data.h(3));
    ROS_WARN("hx  A is :\n%f,%f,%f\n%f,%f,%f\n%f,%f,%f",
             ekfom_data.h_x(0,3),ekfom_data.h_x(0,4),ekfom_data.h_x(0,4),
             ekfom_data.h_x(1,3),ekfom_data.h_x(1,4),ekfom_data.h_x(1,4),
             ekfom_data.h_x(2,3),ekfom_data.h_x(2,5),ekfom_data.h_x(2,5));
    ROS_WARN("hx  B is :\n%f,%f,%f\n%f,%f,%f\n%f,%f,%f",
             ekfom_data.h_x(0,24),ekfom_data.h_x(0,25),ekfom_data.h_x(0,26),
             ekfom_data.h_x(1,24),ekfom_data.h_x(1,25),ekfom_data.h_x(1,26),
             ekfom_data.h_x(2,24),ekfom_data.h_x(2,25),ekfom_data.h_x(2,26));

    }
    else
    {
        double match_start = omp_get_wtime();
        laserCloudOri->clear();
        corr_normvect->clear();
        total_residual = 0.0;

        /** closest surface search and residual computation **/
#ifdef MP_EN
        omp_set_num_threads(MP_PROC_NUM);
#pragma omp parallel for
#endif
        for (int i = 0; i < feats_down_size; i++)
        {
            PointType &point_body  = feats_down_body->points[i];
            PointType &point_world = feats_down_world->points[i];

            /* transform to world frame */
            V3D p_body(point_body.x, point_body.y, point_body.z);
            V3D pc_imu(s.offset_R_L_I*p_body + s.offset_T_L_I);
            V3D p_global(s.rot * (s.offset_R_L_I*p_body + s.offset_T_L_I) + s.pos);
            point_world.x = p_global(0);
            point_world.y = p_global(1);
            point_world.z = p_global(2);
            point_world.intensity = point_body.intensity;


            vector<float> pointSearchSqDis(NUM_MATCH_POINTS);//5点法平面匹配

            auto &points_near = Nearest_Points[i];

            if (ekfom_data.converge)
            {
                /** Find the closest surfaces in the map **/
                ikdtree.Nearest_Search(point_world, NUM_MATCH_POINTS, points_near, pointSearchSqDis);
                point_selected_surf[i] = points_near.size() < NUM_MATCH_POINTS ? false : pointSearchSqDis[NUM_MATCH_POINTS - 1] > 5 ? false : true; //最远匹配点距离平方小于5,搜索5个点
//            :  pointSearchSqDis[0] > 0.2 ? false
                if(point_selected_surf[i]==0)
                {
                    point_world.intensity = 255;
                    point_body.intensity=255;
                    feats_down_body->points[i].intensity=255;
                    feats_down_world->points[i].intensity=255;
                }
                else if(pointSearchSqDis[0] > 0.2)
                {
                    point_world.intensity = 128;
                    point_body.intensity=128;
                    feats_down_body->points[i].intensity=128;
                    feats_down_world->points[i].intensity=128;
                }
                else
                {
                    point_world.intensity = 0;
                    point_body.intensity=0;
                    feats_down_body->points[i].intensity=0;
                    feats_down_world->points[i].intensity=0;
                }
//            if(pc_imu(2)<-1.6&&pc_imu(2)>-1.8)      //不准确，可能包括路沿边，通过ring角度计算准确一些
//        {
//                point_world.intensity = 0;
//                point_body.intensity=0;
//                feats_down_body->points[i].intensity=0;
//                feats_down_world->points[i].intensity=0;
//
//        }
//        else
//        {
//                point_world.intensity = 255;
//                point_body.intensity=255;
//                feats_down_body->points[i].intensity=255;
//                feats_down_world->points[i].intensity=255;
//          //  point_selected_surf[i]=0;
//        }

            }

            if (!point_selected_surf[i]) continue;

            VF(4) pabcd;
            point_selected_surf[i] = false;
            if (esti_plane(pabcd, points_near, 0.1f))//搜索点平面拟合
            {
                float pd2 = pabcd(0) * point_world.x + pabcd(1) * point_world.y + pabcd(2) * point_world.z + pabcd(3);
                float s = 1 - 0.9 * fabs(pd2) / sqrt(p_body.norm());//点面匹配 e/r > 1/9

                if (s > 0.9)
                {
                    point_selected_surf[i] = true;
                    normvec->points[i].x = pabcd(0);
                    normvec->points[i].y = pabcd(1);
                    normvec->points[i].z = pabcd(2);
                    normvec->points[i].intensity = pd2;
                    res_last[i] = abs(pd2);//wheel 为x，y，z绝对值相加？
                }
            }
        }

        effct_feat_num = 0;

        for (int i = 0; i < feats_down_size; i++)
        {
            if (point_selected_surf[i])
            {
                laserCloudOri->points[effct_feat_num] = feats_down_body->points[i];
                corr_normvect->points[effct_feat_num] = normvec->points[i];
                total_residual += res_last[i];
                effct_feat_num ++;
            }
        }

        if (effct_feat_num < 1)
        {
            ekfom_data.valid = false;
            ROS_WARN("No Effective Points! \n");
            return;
        }

        res_mean_last = total_residual / effct_feat_num;//auto jacobian 应当对该项自动求导
        match_time  += omp_get_wtime() - match_start;
        double solve_start_  = omp_get_wtime();
//


        /*** Computation of Measuremnt Jacobian matrix H and measurents vector ***/
        ekfom_data.h_x = MatrixXd::Zero(effct_feat_num, 27); //23
        ekfom_data.R = MatrixXd::Zero(effct_feat_num, effct_feat_num);
        ekfom_data.h.resize(effct_feat_num);

        for (int i = 0; i < effct_feat_num; i++)
        {
            const PointType &laser_p  = laserCloudOri->points[i];
            V3D point_this_be(laser_p.x, laser_p.y, laser_p.z);
            M3D point_be_crossmat;
            point_be_crossmat << SKEW_SYM_MATRX(point_this_be);
            V3D point_this = s.offset_R_L_I * point_this_be + s.offset_T_L_I;
            M3D point_crossmat;
            point_crossmat<<SKEW_SYM_MATRX(point_this);

            /*** get the normal vector of closest surface/corner ***/
            const PointType &norm_p = corr_normvect->points[i];
            V3D norm_vec(norm_p.x, norm_p.y, norm_p.z);

            /*** calculate the Measuremnt Jacobian matrix H ***/
            V3D C(s.rot.conjugate() *norm_vec);
            V3D A(point_crossmat * C);
        ekfom_data.R(i,i)=1e3;
//            ekfom_data.R(i,i)=15000 / (12.7+2.742*sqrt(laser_p.x*laser_p.x + laser_p.y*laser_p.y + laser_p.z*laser_p.z));
            if (extrinsic_est_en)
            {
                V3D B(point_be_crossmat * s.offset_R_L_I.conjugate() * C); //s.rot.conjugate()*norm_vec);
                ekfom_data.h_x.block<1, 12>(i,0) << norm_p.x, norm_p.y, norm_p.z, VEC_FROM_ARRAY(A), VEC_FROM_ARRAY(B), VEC_FROM_ARRAY(C);
            }
            else
            {
                ekfom_data.h_x.block<1, 12>(i,0) << norm_p.x, norm_p.y, norm_p.z, VEC_FROM_ARRAY(A), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0;
            }

            /*** Measuremnt: distance to the closest surface/corner ***/
            ekfom_data.h(i) = -norm_p.intensity;
        }
//    ekfom_data.h_x.block<1, 15>(effct_feat_num+0,0) << 0,0,0, 0,0,0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,1,0,0;
//    ekfom_data.h_x.block<1, 15>(effct_feat_num+1,0) << 0,0,0, 0,0,0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,0,1,0;
//    ekfom_data.h_x.block<1, 15>(effct_feat_num+2,0) << 0,0,0, 0,0,0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,0,0,1;
//    ekfom_data.h(effct_feat_num+0)=-(s.vel.x()- p_imu->wheel_velocity[0]);
//    ekfom_data.h(effct_feat_num+1)=-(s.vel.y()- p_imu->wheel_velocity[1]);
//    ekfom_data.h(effct_feat_num+2)=-(s.vel.z()- p_imu->wheel_velocity[2]);
////    ekfom_data.R.block<3,3>(effct_feat_num,effct_feat_num) = 1e4*MatrixXd::Identity(3, 3);
//    ekfom_data.R.block<3,3>(effct_feat_num,effct_feat_num) = 1e2*MatrixXd::Identity(3, 3);
        solve_time += omp_get_wtime() - solve_start_;
    }
}

int main(int argc, char** argv)
{
    ros::init(argc, argv, "laserMapping");
    ros::NodeHandle nh;

    nh.param<bool>("publish/path_en",path_en, true);
    nh.param<bool>("publish/scan_publish_en",scan_pub_en, true);
    nh.param<bool>("publish/dense_publish_en",dense_pub_en, false);
    nh.param<bool>("publish/scan_bodyframe_pub_en",scan_body_pub_en, true);
    nh.param<int>("max_iteration",NUM_MAX_ITERATIONS,4);
    nh.param<string>("map_file_path",map_file_path,"");
    nh.param<string>("common/lid_topic",lid_topic,"/livox/lidar");
    nh.param<string>("common/lid_r_topic",lid_r_topic,"/livox/lidar");
    nh.param<string>("common/imu_topic", imu_topic,"/livox/imu");
    nh.param<string>("common/wheel_topic", wheel_topic,"/wheel_odom");
    nh.param<string>("common/altimeter_topic", altimeter_topic,"/altimeter_data");
    nh.param<bool>("common/time_sync_en", time_sync_en, false);
    nh.param<double>("filter_size_corner",filter_size_corner_min,0.5);
    nh.param<double>("filter_size_surf",filter_size_surf_min,0.5);
    nh.param<double>("filter_size_map",filter_size_map_min,0.5);
    nh.param<double>("cube_side_length",cube_len,200);
    nh.param<float>("mapping/det_range",DET_RANGE,300.f);
    nh.param<double>("mapping/fov_degree",fov_deg,180);
    nh.param<double>("mapping/gyr_cov",gyr_cov,0.1);
    nh.param<double>("mapping/acc_cov",acc_cov,0.1);
    nh.param<double>("mapping/b_gyr_cov",b_gyr_cov,0.0001);
    nh.param<double>("mapping/b_acc_cov",b_acc_cov,0.0001);
    nh.param<double>("preprocess/blind", p_pre->blind, 0.01);
    nh.param<int>("preprocess/lidar_type", p_pre->lidar_type, AVIA);
    nh.param<int>("preprocess/scan_line", p_pre->N_SCANS, 16);
    nh.param<int>("preprocess/scan_rate", p_pre->SCAN_RATE, 10);
    nh.param<int>("point_filter_num", p_pre->point_filter_num, 2);
    nh.param<bool>("feature_extract_enable", p_pre->feature_enabled, false);
    nh.param<bool>("runtime_pos_log_enable", runtime_pos_log, 0);
    nh.param<bool>("mapping/extrinsic_est_en", extrinsic_est_en, true);
    nh.param<bool>("pcd_save/pcd_save_en", pcd_save_en, false);
    nh.param<int>("pcd_save/interval", pcd_save_interval, -1);
    nh.param<vector<double>>("mapping/extrinsic_T", extrinT, vector<double>());
    nh.param<vector<double>>("mapping/extrinsic_R", extrinR, vector<double>());
    nh.param<vector<double>>("mapping/r2l_extrinsic_T", r2l_extrinT, vector<double>());
    nh.param<vector<double>>("mapping/r2l_extrinsic_R", r2l_extrinR, vector<double>());
    nh.param<vector<double>>("chassis/extrinsic_T_W", extrinT_W, vector<double>());
    nh.param<vector<double>>("chassis/extrinsic_R_W", extrinR_W, vector<double>());
    nh.param<double>("chassis/wheel_cov",wheel_cov,1e-4);
    nh.param<double>("chassis/left_wheel_diameter",left_wheel_diameter,0.623479);
    nh.param<double>("chassis/right_wheel_diameter",right_wheel_diameter,0.622806);
    nh.param<double>("chassis/wheel_base",wheel_base,1.52439);

    cout<<"p_pre->lidar_type "<<p_pre->lidar_type<<endl;
    
    path.header.stamp    = ros::Time::now();
    path.header.frame_id ="camera_init";

    /*** variables definition ***/
    int effect_feat_num = 0, frame_num = 0;//各部分时间
    double deltaT, deltaR, aver_time_consu = 0, aver_time_icp = 0, aver_time_match = 0, aver_time_incre = 0, aver_time_solve = 0, aver_time_const_H_time = 0;
    bool flg_EKF_converged, EKF_stop_flg = 0;   //判断IEKF是否收敛
    
    FOV_DEG = (fov_deg + 10.0) > 179.9 ? 179.9 : (fov_deg + 10.0);
    HALF_FOV_COS = cos((FOV_DEG) * 0.5 * PI_M / 180.0);

    _featsArray.reset(new PointCloudXYZI());

    memset(point_selected_surf, true, sizeof(point_selected_surf));//选取匹配平面的点云
    memset(res_last, -1000.0f, sizeof(res_last));//残差负大值
    downSizeFilterSurf.setLeafSize(filter_size_surf_min, filter_size_surf_min, filter_size_surf_min);
    downSizeFilterMap.setLeafSize(filter_size_map_min, filter_size_map_min, filter_size_map_min);
    memset(point_selected_surf, true, sizeof(point_selected_surf));
    memset(res_last, -1000.0f, sizeof(res_last));

    Lidar_T_wrt_IMU<<VEC_FROM_ARRAY(extrinT);
    Lidar_R_wrt_IMU<<MAT_FROM_ARRAY(extrinR);
    Wheel_T_wrt_IMU<<VEC_FROM_ARRAY(extrinT_W);
    Wheel_R_wrt_IMU<<MAT_FROM_ARRAY(extrinR_W);
    p_imu->set_extrinsic(Lidar_T_wrt_IMU, Lidar_R_wrt_IMU, Wheel_T_wrt_IMU, Wheel_R_wrt_IMU);
    p_imu->set_gyr_cov(V3D(gyr_cov, gyr_cov, gyr_cov));
    p_imu->set_acc_cov(V3D(acc_cov, acc_cov, acc_cov));
    p_imu->set_gyr_bias_cov(V3D(b_gyr_cov, b_gyr_cov, b_gyr_cov));
    p_imu->set_acc_bias_cov(V3D(b_acc_cov, b_acc_cov, b_acc_cov));

    double epsi[29] = {0.001};//误差项初始化，23=24-1个state,gravity S2,判断收敛条件
    fill(epsi, epsi+29, 0.001);
    kf.init_dyn_share(get_f, df_dx, df_dw, h_share_model, NUM_MAX_ITERATIONS, epsi);
//    kf.init_dyn_share(get_f, df_dx, df_dw, h_share_model_wheel, NUM_MAX_ITERATIONS, epsi);

    /*** debug record ***/
    FILE *fp,*fp3;




    string pos_log_dir = root_dir + "/Log/pos_log.txt";
    fp = fopen(pos_log_dir.c_str(),"w");

    string evo_pose_dir = root_dir + "/Log/evo_pose.txt";
    fp3 = fopen(evo_pose_dir.c_str(),"w");

    ofstream fout_pre, fout_out, fout_dbg;
    fout_pre.open(DEBUG_FILE_DIR("mat_pre.txt"),ios::out);
    fout_out.open(DEBUG_FILE_DIR("mat_out.txt"),ios::out);
    fout_dbg.open(DEBUG_FILE_DIR("mat_dbg.txt"),ios::out);
    if (fout_pre && fout_out && fout_dbg)
        cout << "~~~~"<<ROOT_DIR<<" file opened" << endl;
    else
        cout << "~~~~"<<ROOT_DIR<<" doesn't exist" << endl;

    /*** ROS subscribe initialization ***/
    ros::Subscriber sub_pcl = p_pre->lidar_type == AVIA ? \
        nh.subscribe(lid_topic, 200000, livox_pcl_cbk) : \
        nh.subscribe(lid_topic, 200000, standard_pcl_cbk);
    ros::Subscriber sub_pcl_r = p_pre->lidar_type == AVIA ? \
        nh.subscribe(lid_r_topic, 200000, livox_pcl_cbk) : \
        nh.subscribe(lid_r_topic, 200000, standard_pcl_r_cbk);


//    message_filters::Subscriber<sensor_msgs::PointCloud2> l_lidar_sub;//雷达订阅
//    message_filters::Subscriber<sensor_msgs::PointCloud2> r_lidar_sub;
//    typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::PointCloud2, sensor_msgs::PointCloud2> syncpolicy;//时间戳对齐规则
//    typedef message_filters::Synchronizer<syncpolicy> Sync;
//    boost::shared_ptr<Sync> sync_;//时间同步器
//    l_lidar_sub.subscribe(nh,lid_topic, 200000);
//    r_lidar_sub.subscribe(nh,lid_r_topic, 200000);
//    sync_.reset(new Sync(syncpolicy(10), l_lidar_sub, r_lidar_sub));
//    sync_->registerCallback(boost::bind(&standard_pcl2_cbk, _1, _2));
   // sync_->registerCallback(boost::bind(&standard_pcl_cbk, _1));

    ros::Subscriber sub_imu = nh.subscribe(imu_topic, 200000, imu_cbk,ros::TransportHints().tcpNoDelay());
    ros::Subscriber sub_wheel = nh.subscribe(wheel_topic, 200000, wheel_cbk, ros::TransportHints().tcpNoDelay());
//    ros::Subscriber sub_altimeter = nh.subscribe(altimeter_topic, 200000, altimeter_cbk, ros::TransportHints().tcpNoDelay());
//    ros::Subscriber sub_wheel = nh.subscribe(wheel_topic, 200000, wheel_cbk,ros::TransportHints().tcpNoDelay());
    ros::Publisher pubLaserCloudFull = nh.advertise<sensor_msgs::PointCloud2>
            ("/cloud_registered", 100000);
    ros::Publisher pubLaserCloudFull_body = nh.advertise<sensor_msgs::PointCloud2>
            ("/cloud_registered_body", 100000);
    ros::Publisher pubLaserCloudEffect = nh.advertise<sensor_msgs::PointCloud2>
            ("/cloud_effected", 100000);
    ros::Publisher pubLaserCloudMap = nh.advertise<sensor_msgs::PointCloud2>
            ("/Laser_map", 100000);
    ros::Publisher pubOdomAftMapped = nh.advertise<nav_msgs::Odometry> 
            ("/Odometry", 100000);
    ros::Publisher pubPath          = nh.advertise<nav_msgs::Path> 
            ("/path", 100000);

    ros::Publisher pubLaserFull = nh.advertise<sensor_msgs::PointCloud2>
            ("/cloud_full", 100000);
    ros::Publisher pubLaserCor = nh.advertise<sensor_msgs::PointCloud2>
            ("/cloud_cor", 100000);
//------------------------------------------------------------------------------------------------------
    signal(SIGINT, SigHandle);
    ros::Rate rate(5000);// 0.2ms
    bool status = ros::ok();
    while (status)
    {
        if (flg_exit) break;
        ros::spinOnce();
        if(sync_packages(Measures)) //时间同步
        {
            if (flg_first_scan)
            {
                first_lidar_time = Measures.lidar_beg_time;
                p_imu->first_lidar_time = first_lidar_time;
                flg_first_scan = false;
                continue;
            }

            double t0,t1,t2,t3,t4,t5,match_start, solve_start, svd_time;

            match_time = 0;
            kdtree_search_time = 0.0;
            solve_time = 0;
            solve_const_H_time = 0;
            svd_time   = 0;
            t0 = omp_get_wtime();

            p_imu->Process(Measures, kf, feats_undistort);//IMU预积分，点云去畸变
            state_point = kf.get_x();
            pos_lid = state_point.pos + state_point.rot * state_point.offset_T_L_I; //Rp+t

            if (feats_undistort->empty() || (feats_undistort == NULL))
            {
                ROS_WARN("No point, skip this scan!\n");
                continue;
            }

            flg_EKF_inited = (Measures.lidar_beg_time - first_lidar_time) < INIT_TIME ? \
                            false : true;
            /*** Segment the map in lidar FOV ***/
            lasermap_fov_segment(); //分割出range和fov的局部地图

            /*** downsample the feature points in a scan ***/
            downSizeFilterSurf.setInputCloud(feats_undistort);
            downSizeFilterSurf.filter(*feats_down_body);
            t1 = omp_get_wtime();
            feats_down_size = feats_down_body->points.size();
            /*** initialize the map kdtree ***/
            if(ikdtree.Root_Node == nullptr)
            {
                if(feats_down_size > 5)
                {
                    ikdtree.set_downsample_param(filter_size_map_min);
                    feats_down_world->resize(feats_down_size);
                    for(int i = 0; i < feats_down_size; i++)
                    {
                        pointBodyToWorld(&(feats_down_body->points[i]), &(feats_down_world->points[i]));
                    }
                    ikdtree.Build(feats_down_world->points);
                }
                continue;
            }
            int featsFromMapNum = ikdtree.validnum();
            kdtree_size_st = ikdtree.size();

            // cout<<"[ mapping ]: In num: "<<feats_undistort->points.size()<<" downsamp "<<feats_down_size<<" Map num: "<<featsFromMapNum<<"effect num:"<<effct_feat_num<<endl;

            /*** ICP and iterated Kalman filter update ***/
            if (feats_down_size < 5)
            {
                ROS_WARN("No point, skip this scan!\n");
                continue;
            }

            normvec->resize(feats_down_size);
            feats_down_world->resize(feats_down_size);

            V3D ext_euler = SO3ToEuler(state_point.offset_R_W_I);
            fout_pre<<setw(20)<<Measures.lidar_beg_time - first_lidar_time<<" "<<euler_cur.transpose()<<" "<< state_point.pos.transpose()<<" "<<ext_euler.transpose() << " "<<state_point.offset_T_L_I.transpose()<< " " << state_point.vel.transpose() \
            <<" "<<state_point.bg.transpose()<<" "<<state_point.ba.transpose()<<" "<<state_point.grav<< endl;

            if(0) // If you need to see map point, change to "if(1)"
            {
                PointVector ().swap(ikdtree.PCL_Storage);
                ikdtree.flatten(ikdtree.Root_Node, ikdtree.PCL_Storage, NOT_RECORD);
                featsFromMap->clear();
                featsFromMap->points = ikdtree.PCL_Storage;
            }

            pointSearchInd_surf.resize(feats_down_size);
            Nearest_Points.resize(feats_down_size);
            int  rematch_num = 0;
            bool nearest_search_en = true; //

            t2 = omp_get_wtime();

            /*** iterated state estimation ***/
            double t_update_start = omp_get_wtime();
            double solve_H_time = 0;
            kf.update_iterated_dyn_share_modified(LASER_POINT_COV, solve_H_time);
//            kf.update_iterated_dyn_share_modified(1e-6, solve_H_time);
            state_point = kf.get_x();
            euler_cur = SO3ToEuler(state_point.rot);
            pos_lid = state_point.pos + state_point.rot * state_point.offset_T_L_I;
            geoQuat.x = state_point.rot.coeffs()[0];
            geoQuat.y = state_point.rot.coeffs()[1];
            geoQuat.z = state_point.rot.coeffs()[2];
            geoQuat.w = state_point.rot.coeffs()[3];

            double t_update_end = omp_get_wtime();

            /******* Publish odometry *******/
            publish_odometry(pubOdomAftMapped,fp3);

            /*** add the feature points to map kdtree ***/
            t3 = omp_get_wtime();
            map_incremental();      //ikd增量建图
            t5 = omp_get_wtime();

            /******* Publish points *******/
            if (path_en)                         publish_path(pubPath);
            if (scan_pub_en || pcd_save_en)      publish_frame_world(pubLaserCloudFull,pubLaserCor,pubLaserFull);
            if (scan_pub_en && scan_body_pub_en) publish_frame_body(pubLaserCloudFull_body);
             publish_effect_world(pubLaserCloudEffect);
             publish_map(pubLaserCloudMap);

            /*** Debug variables ***/
            if (runtime_pos_log)
            {
                frame_num ++;
                kdtree_size_end = ikdtree.size();
                aver_time_consu = aver_time_consu * (frame_num - 1) / frame_num + (t5 - t0) / frame_num;
                aver_time_icp = aver_time_icp * (frame_num - 1)/frame_num + (t_update_end - t_update_start) / frame_num;
                aver_time_match = aver_time_match * (frame_num - 1)/frame_num + (match_time)/frame_num;
                aver_time_incre = aver_time_incre * (frame_num - 1)/frame_num + (kdtree_incremental_time)/frame_num;
                aver_time_solve = aver_time_solve * (frame_num - 1)/frame_num + (solve_time + solve_H_time)/frame_num;
                aver_time_const_H_time = aver_time_const_H_time * (frame_num - 1)/frame_num + solve_time / frame_num;
                T1[time_log_counter] = Measures.lidar_beg_time;
                s_plot[time_log_counter] = t5 - t0;
                s_plot2[time_log_counter] = feats_undistort->points.size();
                s_plot3[time_log_counter] = kdtree_incremental_time;
                s_plot4[time_log_counter] = kdtree_search_time;
                s_plot5[time_log_counter] = kdtree_delete_counter;
                s_plot6[time_log_counter] = kdtree_delete_time;
                s_plot7[time_log_counter] = kdtree_size_st;
                s_plot8[time_log_counter] = kdtree_size_end;
                s_plot9[time_log_counter] = aver_time_consu;
                s_plot10[time_log_counter] = add_point_size;
                time_log_counter ++;
//                printf("[ mapping ]: time: IMU + Map + Input Downsample: %0.6f ave match: %0.6f ave solve: %0.6f  ave ICP: %0.6f  map incre: %0.6f ave total: %0.6f icp: %0.6f construct H: %0.6f \n",t1-t0,aver_time_match,aver_time_solve,t3-t1,t5-t3,aver_time_consu,aver_time_icp, aver_time_const_H_time);
                ext_euler = SO3ToEuler(state_point.offset_R_W_I);
                fout_out << setw(20) << Measures.lidar_beg_time - first_lidar_time << " " << euler_cur.transpose() << " " << state_point.pos.transpose()<< " " << ext_euler.transpose() << " "<<state_point.offset_T_L_I.transpose()<<" "<< state_point.vel.transpose() \
                <<" "<<state_point.bg.transpose()<<" "<<state_point.ba.transpose()<<" "<<state_point.grav<<" "<<feats_undistort->points.size()<<endl;
//                fout_out << setw(20) << Measures.lidar_beg_time - first_lidar_time << " " << euler_cur[0] << " "<< euler_cur[1] << " "<< euler_cur[2] << " " << state_point.pos.x()<< " "\
//                <<state_point.pos.y()<< " " <<p_imu->altimeter_height<< " " \
//                << imu_buffer.back()->angular_velocity.x  << " "<< imu_buffer.back()->angular_velocity.y << " "<< imu_buffer.back()->angular_velocity.z << " "\
//                <<imu_buffer.back()->linear_acceleration.x<<" " <<imu_buffer.back()->linear_acceleration.y<<" " <<imu_buffer.back()->linear_acceleration.z<<" "<< p_imu->wheel_velocity.transpose() \
//                <<" "<<state_point.bg.transpose()<<" "<<state_point.ba.transpose()<<" "<<state_point.grav<<" "<<feats_undistort->points.size()<<endl;
//
//                fout_dbg << setw(20) << Measures.lidar_beg_time - first_lidar_time << " " << euler_cur[0] << " "<< euler_cur[1] << " "<< euler_cur[2] << " " << state_point.pos.transpose()<< " " \
//                << imu_buffer.back()->angular_velocity.x  << " "<< imu_buffer.back()->angular_velocity.y << " "<< imu_buffer.back()->angular_velocity.z << " "\
//                <<imu_buffer.back()->linear_acceleration.x<<" " <<imu_buffer.back()->linear_acceleration.y<<" " <<imu_buffer.back()->linear_acceleration.z<<" "<< state_point.vel.transpose() \
//                <<" "<<state_point.bg.transpose()<<" "<<state_point.ba.transpose()<<" "<<state_point.grav<<" "<<feats_undistort->points.size()<<endl;

                dump_lio_state_to_log(fp);
            }
        }

        if(!wheel_buffer.empty())
        {
            double wheel_time = wheel_buffer.front()->header.stamp.toSec();
            double solve_H_time=0;
//            Measures.wheel.clear();
            while ((!wheel_buffer.empty())&&(wheel_time < lidar_end_time+0.1))
            {
                wheel_time = wheel_buffer.front()->header.stamp.toSec();
                if(wheel_time > lidar_end_time) break;
//                Measures.wheel.push_back(wheel_buffer.front());
                SO3 wheel_rot;
                nav_msgs::Odometry::ConstPtr temp_wheel=wheel_buffer.front();
                wheel_angvel_W << temp_wheel->twist.twist.angular.x, temp_wheel->twist.twist.angular.y, temp_wheel->twist.twist.angular.z;
                wheel_velocity_W<<temp_wheel->twist.twist.linear.x,temp_wheel->twist.twist.linear.y,temp_wheel->twist.twist.linear.z;

//                wheel_velocity_I=state_point.rot*state_point.offset_R_W_I*wheel_velocity_W;
                wheel_buffer.pop_front();
                wheel_update_flag=1;
                kf.update_iterated_dyn_share_modified(LASER_POINT_COV, solve_H_time);
                wheel_update_flag=0;
            }
        }

        status = ros::ok();
        rate.sleep();
    }

    /**************** save map ****************/
    /* 1. make sure you have enough memories
    /* 2. pcd save will largely influence the real-time performences **/
    if (pcl_wait_save->size() > 0 && pcd_save_en)
    {
        string file_name = string("scans.pcd");
        string all_points_dir(string(string(ROOT_DIR) + "PCD/") + file_name);
        pcl::PCDWriter pcd_writer;
        cout << "current scan saved to /PCD/" << file_name<<endl;
        pcd_writer.writeBinary(all_points_dir, *pcl_wait_save);
    }

    fout_out.close();
    fout_pre.close();
    fout_dbg.close();

    if (runtime_pos_log)
    {
        vector<double> t, s_vec, s_vec2, s_vec3, s_vec4, s_vec5, s_vec6, s_vec7;
        FILE *fp2;
        string log_dir = root_dir + "/Log/fast_lio_time_log.csv";
        fp2 = fopen(log_dir.c_str(),"w");
        fprintf(fp2,"time_stamp, total time, scan point size, incremental time, search time, delete size, delete time, tree size st, tree size end, add point size, preprocess time\n");
        for (int i = 0;i<time_log_counter; i++){
            fprintf(fp2,"%0.8f,%0.8f,%d,%0.8f,%0.8f,%d,%0.8f,%d,%d,%d,%0.8f\n",T1[i],s_plot[i],int(s_plot2[i]),s_plot3[i],s_plot4[i],int(s_plot5[i]),s_plot6[i],int(s_plot7[i]),int(s_plot8[i]), int(s_plot10[i]), s_plot11[i]);
            t.push_back(T1[i]);
            s_vec.push_back(s_plot9[i]);
            s_vec2.push_back(s_plot3[i] + s_plot6[i]);
            s_vec3.push_back(s_plot4[i]);
            s_vec5.push_back(s_plot[i]);
        }
        fclose(fp2);
    }

    return 0;
}
