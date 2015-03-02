#include "ed_sensor_integration/kinect/almodules/relative_alm.h"

#include <rgbd/Image.h>
#include <rgbd/View.h>
#include <opencv/highgui.h>
#include <pcl/registration/icp.h>

#include <ed/helpers/depth_data_processing.h>
#include <ed/helpers/visualization.h>
#include <ed/entity.h>

#include "ed_sensor_integration/kinect/almodules/world_model_renderer.h"

#include <tue/profiling/scoped_timer.h>

#include <geolib/ros/tf_conversions.h>

namespace edKinect
{

RelativeLocalizationModule::RelativeLocalizationModule() : RGBDALModule("relative_localization")
{
}

void RelativeLocalizationModule::configure(tue::Configuration config)
{
    if (config.readGroup("parameters"))
    {
        config.value("association_correspondence_distance", association_correspondence_distance_);
        config.value("position_weight", position_weight_);
        config.value("normal_weight", normal_weight_);
        config.value("visualize", visualize_);
        config.value("render_width", render_width_);
        config.value("render_max_range", render_max_range_);
        config.value("render_voxel_size", render_voxel_size_);
        config.value("normal_k_search", normal_k_search_);

        std::cout << "Parameters relative localization association module: \n" <<
                     "- association_correspondence_distance: " << association_correspondence_distance_ << "\n" <<
                     "- position_weight: " << position_weight_ << "\n" <<
                     "- normal_weight: " << normal_weight_ << "\n" <<
                     "- render_width: " << render_width_ << "\n" <<
                     "- render_max_range: " << render_max_range_ << "\n" <<
                     "- render_voxel_size_: " << render_voxel_size_ << "\n" <<
                     "- normal_k_search: " << normal_k_search_ << "\n" <<
                     "- visualize: " << visualize_ << std::endl;

        config.endGroup();
    }

    float pw = position_weight_;
    float cw = normal_weight_;
    float alpha[6] = {pw,pw,pw,cw,cw,cw};
    point_representation_.setRescaleValues (alpha);

    tree_ = pcl::KdTreeFLANN<pcl::PointNormal>::Ptr(new pcl::KdTreeFLANN<pcl::PointNormal>);
}

geo::Transform RelativeLocalizationModule::eigenMat2geoTransform(Eigen::Matrix<float,4,4> T) {
    geo::Transform Transformation;

    Transformation.R.xx = T(0,0);
    Transformation.R.xy = T(0,1);
    Transformation.R.xz = T(0,2);

    Transformation.R.yx = T(1,0);
    Transformation.R.yy = T(1,1);
    Transformation.R.yz = T(1,2);

    Transformation.R.zx = T(2,0);
    Transformation.R.zy = T(2,1);
    Transformation.R.zz = T(2,2);

    Transformation.t.x = T(0,3);
    Transformation.t.y = T(1,3);
    Transformation.t.z = T(2,3);
    return Transformation;
}

void RelativeLocalizationModule::process(ed::RGBDData& sensor_data,
                                         ed::PointCloudMaskPtr& not_associated_mask,
                                         const ed::WorldModel& world_model,
                                         ALMResult& result)
{

    //! 1) Get the world model point cloud
    pcl::PointCloud<pcl::PointNormal>::ConstPtr world_model_npcl;
    std::vector<const ed::Entity*> world_model_pc_entity_ptrs;
    // Create a render view
    rgbd::View sensor_view(*sensor_data.image, render_width_);

    // Render the view
    WorldModelRenderer wmr;
    cv::Mat wm_depth_img = cv::Mat::zeros(sensor_view.getHeight(), sensor_view.getWidth(), CV_32F);
    pcl::PointCloud<pcl::PointXYZ>::Ptr world_model_pcl(new pcl::PointCloud<pcl::PointXYZ>);
    wmr.render(sensor_data.sensor_pose, world_model, render_max_range_, sensor_view, wm_depth_img, *world_model_pcl, world_model_pc_entity_ptrs);

    // Downsample the pointcloud (why?)
    pcl::PointCloud<pcl::PointXYZ>::ConstPtr world_model_pcl_downsampled = ed::helpers::ddp::downSamplePcl(world_model_pcl, render_voxel_size_);

    // Calculate the normals with use of pcl
    world_model_npcl =  ed::helpers::ddp::pclToNpcl(world_model_pcl_downsampled, normal_k_search_);

    if (visualize_) {
        //        ed::helpers::visualization::publishNpclVisualizationMarker(sensor_data.sensor_pose, world_model_npcl, vis_marker_pub_, 1, "world_model_npc");
        ed::helpers::visualization::publishPclVisualizationMarker(sensor_data.sensor_pose, world_model_pcl_downsampled, vis_marker_pub_, 1, "world_model_pc");
    }

    //! 3.1) Match all depth data with world model render to update sensor pose
    //    pcl::PointCloud<pcl::PointNormal> final_pc;
    pcl::PointCloud<pcl::PointNormal>::Ptr final_pc(new pcl::PointCloud<pcl::PointNormal>);

    pcl::IterativeClosestPoint<pcl::PointNormal, pcl::PointNormal> icp;
    icp.setInputSource(sensor_data.point_cloud_with_normals);
    icp.setInputTarget(world_model_npcl);
    icp.align(*final_pc);

    //    std::cout << "Original pose = " << sensor_data.sensor_pose << std::endl;
    if (icp.hasConverged()) {
        Eigen::Matrix<float, 4, 4> T = icp.getFinalTransformation();
        geo::Pose3D pose_correction = RelativeLocalizationModule::eigenMat2geoTransform(T);
        //        std::cout << "Pose correction = " << pose_correction << std::endl;
        result.sensor_pose_corrected = sensor_data.sensor_pose * pose_correction;
        //        std::cout << "Corrected pose = " << result.sensor_pose_corrected << "\n" << std::endl;
        if (visualize_) {
            ed::helpers::visualization::publishRGBDViewFrustrumVisualizationMarker(sensor_view, result.sensor_pose_corrected, vis_marker_pub_, 2, "corrected_pose");
            ed::helpers::visualization::publishPclVisualizationMarker(result.sensor_pose_corrected, final_pc, vis_marker_pub_, 2, "corrected_pc");
        }

        // Convert back from Geolib frame to ROS frame
        result.sensor_pose_corrected.R = result.sensor_pose_corrected.R * geo::Matrix3(1, 0, 0, 0, -1, 0, 0, 0, -1);
    }

    if (visualize_) {

        ed::helpers::visualization::publishRGBDViewFrustrumVisualizationMarker(sensor_view, sensor_data.sensor_pose, vis_marker_pub_, 1, "original_pose");
    }

    //! 2) Perform point normal association
    {
        //    // Create vector of pointers to entities with which the data points are associated.
        //    std::vector<const ed::Entity*> associations(sensor_data.point_cloud_with_normals->size(),0);
        //    std::map< const ed::Entity*, pcl::PointCloud<pcl::PointNormal>::Ptr > sensor_association_map, wm_association_map;

        //    {
        //        tue::ScopedTimer t(profiler_, "2) association");

        //        // If something went wrong, return
        //        if (world_model_npcl->size() == 0 || !tree_)
        //            return;

        //        {
        //            tue::ScopedTimer setinputcloud_timer(profiler_, "setinputcloud");
        //            // Set point representation of KdTree and set input point cloud (world model point cloud with normals)
        //            tree_->setPointRepresentation (boost::make_shared<const AssociationPRRelativeALM> (point_representation_));
        //            tree_->setInputCloud(world_model_npcl);
        //        }

        //        // Loop over (empty) vector of associated points
        //        for (unsigned int i = 0; i < associations.size(); ++i)
        //        {
        //            // Declaration of vector to contain index to a point in the world model that is associated with point i in the sensor data
        //            std::vector<int> k_indices(1);
        //            // Declaration of vector to contain the squared distance between those points
        //            std::vector<float> k_sqr_distances(1);

        //            // Perform the radius search in the world model point cloud from point i in the sensor data point cloud
        //            if (tree_->radiusSearch(*sensor_data.point_cloud_with_normals, i, association_correspondence_distance_, k_indices, k_sqr_distances, 1))
        //            {
        //                // Store pointer to associated entity in associations
        //                associations[i] = world_model_pc_entity_ptrs[k_indices[0]];
        //                if (!sensor_association_map[associations[i]] && !wm_association_map[associations[i]])
        //                {
        //                    // Create new point clouds to save associated entity points in
        //                    pcl::PointCloud<pcl::PointNormal>::Ptr ptr_to_empty_pc1(new pcl::PointCloud<pcl::PointNormal>);
        //                    pcl::PointCloud<pcl::PointNormal>::Ptr ptr_to_empty_pc2(new pcl::PointCloud<pcl::PointNormal>);

        //                    // Add these empty point clouds to a (new) map entry with entity pointer as key
        //                    sensor_association_map[associations[i]] = ptr_to_empty_pc1;
        //                    wm_association_map[associations[i]] = ptr_to_empty_pc2;
        //                }
        //                *sensor_association_map[associations[i]] += pcl::PointCloud<pcl::PointNormal>(*sensor_data.point_cloud_with_normals,std::vector<int>(1,i));
        //                *wm_association_map[associations[i]] += pcl::PointCloud<pcl::PointNormal>(*world_model_npcl,k_indices);
        //            }
        //            else {
        //                // If sensor data point is not associated...
        //                associations[i] = 0;
        //            }
        //        }
        //    }

        //    ! 3) Match scan data with their respective entities in the world model and get transforms from that


        //    ! 3.2) Match separte entity point cloud data with associated world model points to update entity poses
        //    for (std::map<const ed::Entity*, pcl::PointCloud<pcl::PointNormal>::Ptr >::iterator it = sensor_association_map.begin(); it != sensor_association_map.end(); it++) {
        //        const ed::Entity* entity = it->first;

        //        pcl::IterativeClosestPoint<pcl::PointNormal, pcl::PointNormal> icp;
        //        icp.setInputSource(it->second);
        //        icp.setInputTarget(wm_association_map[entity]);
        //        pcl::PointCloud<pcl::PointNormal> final_pc;
        //        icp.align(final_pc);

        //        Eigen::Matrix<float, 4, 4> T = icp.getFinalTransformation();
        //        std::cout << "Transformation matrix T = \n" << T << std::endl;
    }

    //! 4) Update the mask
    {
        tue::ScopedTimer t(profiler_, "mask_update");

        not_associated_mask->clear();

        //        for(unsigned int i = 0; i < associations.size(); ++i)
        //            if (!associations[i])
        //                not_associated_mask->push_back(i);

        //        if (visualize_)
        //        {
        //            pcl::PointCloud<pcl::PointNormal>::Ptr residual_npcl(new pcl::PointCloud<pcl::PointNormal>);
        //            for(unsigned int i = 0; i < associations.size(); ++i)
        //                if (!associations[i])
        //                    residual_npcl->push_back(sensor_data.point_cloud_with_normals->points[i]);

        //            ed::helpers::visualization::publishNpclVisualizationMarker(sensor_data.sensor_pose, residual_npcl, vis_marker_pub_, 2, "residual_npcl");
        //        }
    }

    pub_profile_.publish();
} // process method

} // namespace ed
