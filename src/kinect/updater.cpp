#include "ed/kinect/updater.h"

#include <ed/world_model.h>
#include <ed/entity.h>
#include <ed/update_request.h>

#include <ed/helpers/depth_data_processing.h>

#include <rgbd/View.h>

#include <tue/config/reader.h>

#include <geolib/Shape.h>
#include <geolib/shapes.h>

#include <ed/serialization/serialization.h>

#include "ed/kinect/association.h"
#include "ed/kinect/renderer.h"

#include "ed/convex_hull_calc.h"

#include <opencv2/highgui/highgui.hpp>

#include <ros/console.h>

#include <cmath>

#include "ed/kinect/math_helper.h"


// ----------------------------------------------------------------------------------------------------

// Calculates which depth points are in the given convex hull (in the EntityUpdate), updates the mask,
// and updates the convex hull height based on the points found
void refitConvexHull(const rgbd::Image& image, const geo::Pose3D& sensor_pose, const geo::DepthCamera& cam_model,
                     const Segmenter& segmenter_, EntityUpdate& up)
{
    up.pose_map.t.z += (up.chull.z_max + up.chull.z_min) / 2;

    geo::Shape chull_shape;
    std::vector<geo::Vec2> points(up.chull.points.size());
    for(unsigned int i = 0; i < points.size(); ++i)
        points[i] = geo::Vec2(up.chull.points[i].x, up.chull.points[i].y);
    geo::createConvexPolygon(chull_shape, points, up.chull.height());

    cv::Mat filtered_depth_image;
    segmenter_.calculatePointsWithin(image, chull_shape, sensor_pose.inverse() * up.pose_map, filtered_depth_image);

    up.points.clear();
    up.pixel_indices.clear();

    float z_min =  1e9;
    float z_max = -1e9;

    int i_pixel = 0;
    for(int y = 0; y < filtered_depth_image.rows; ++y)
    {
        for(int x = 0; x < filtered_depth_image.cols; ++x)
        {
            float d = filtered_depth_image.at<float>(y, x);
            if (d == 0)
            {
                ++i_pixel;
                continue;
            }

            geo::Vec3 p = cam_model.project2Dto3D(x, y) * d;
            geo::Vec3 p_map = sensor_pose * p;

            z_min = std::min<float>(z_min, p_map.z);
            z_max = std::max<float>(z_max, p_map.z);

            up.pixel_indices.push_back(i_pixel);
            up.points.push_back(p);
            ++i_pixel;
        }
    }

    double h = z_max - z_min;
    up.pose_map.t.z = (z_max + z_min) / 2;
    up.chull.z_min = -h / 2;
    up.chull.z_max =  h / 2;
}

// ----------------------------------------------------------------------------------------------------
/**
 * @brief mergeConvexHulls, creating a new convexHull around the two objects. Working in both XY and Z.
 * @param u1 EntityUpdate used as starting point
 * @param u2 Merge points into u1
 * @return new EntityUpdate including new convexHull and measurement points of both inputs.
 */
EntityUpdate mergeConvexHulls(const rgbd::Image& image, const geo::Pose3D& sensor_pose, const geo::DepthCamera& cam_model,
                              const Segmenter& segmenter_, const EntityUpdate& u1, const EntityUpdate& u2)
{
    EntityUpdate new_u = u1;
    double z_max = std::max(u1.pose_map.t.getZ()+u1.chull.z_max,u2.pose_map.t.getZ()+u2.chull.z_max);
    double z_min = std::min(u1.pose_map.t.getZ()+u1.chull.z_min,u2.pose_map.t.getZ()+u2.chull.z_min);

    std::vector<geo::Vec2f> points(u1.chull.points.size()+u2.chull.points.size());
    for (unsigned int p = 0; p < u1.chull.points.size(); ++p)
    {
        geo::Vec3 p_map = u1.pose_map * geo::Vec3(u1.chull.points[p].x, u1.chull.points[p].y, 0);
        points[p] = geo::Vec2f(p_map.x, p_map.y);
    }
    unsigned int offset = u1.chull.points.size();
    for (unsigned int p = 0; p < u2.chull.points.size(); ++p)
    {
        geo::Vector3 p_map = u2.pose_map * geo::Vec3(u2.chull.points[p].x, u2.chull.points[p].y, 0);
        points[p + offset] = geo::Vec2f(p_map.x, p_map.y);
    }

    ed::convex_hull::create(points, z_min, z_max, new_u.chull, new_u.pose_map);
    refitConvexHull(image, sensor_pose, cam_model, segmenter_, new_u);

    return new_u;
}

// ----------------------------------------------------------------------------------------------------

// Calculates which depth points are in the given convex hull (in the EntityUpdate), updates the mask,
// and updates the convex hull height based on the points found
std::vector<EntityUpdate> mergeOverlappingConvexHulls(const rgbd::Image& image, const geo::Pose3D& sensor_pose, const geo::DepthCamera& cam_model,
                                                         const Segmenter& segmenter_, const std::vector<EntityUpdate>& updates)
{

  ROS_INFO("mergoverlapping chulls: nr of updates: %lu", updates.size());

  // Updated convex hulls
  std::vector<EntityUpdate> new_updates;

  // Keep track of indices that did collide, skip them in i
  std::vector<int> collided_indices;
  std::map<int, std::vector<int> > collission_map;

  for (int i = 0; i < updates.size(); ++i)
  {
    const EntityUpdate& u1 = updates[i];

    // If index already collided, it will be merged to another one
    if (std::find(collided_indices.begin(), collided_indices.end(), i) != collided_indices.end())
    {
      continue;
    }

    for (int j = 0; j < updates.size(); ++j)
    {
    // skip self
        if (i == j)
            continue;

      const EntityUpdate& u2 = updates[j];

      // If we collide, update the i convex hull
      if (ed::convex_hull::collide(u1.chull, u1.pose_map.t, u2.chull, u2.pose_map.t, 0, 1e6))  // This should prevent multiple entities above each other;1e6 is ok, because objects in other areas are ignored.
      {
        ROS_DEBUG("Collition item %i with %i", i, j);
        ROS_DEBUG("Item %i: xyz: %.2f, %.2f, %.2f, z_min: %.2f, z_max: %.2f", i, u1.pose_map.t.getX(), u1.pose_map.t.getY(), u1.pose_map.t.getZ(), u1.chull.z_min, u1.chull.z_max);
        ROS_DEBUG("Item %i: xyz: %.2f, %.2f, %.2f, z_min: %.2f, z_max: %.2f", j, u2.pose_map.t.getX(), u2.pose_map.t.getY(), u2.pose_map.t.getZ(), u2.chull.z_min, u2.chull.z_max);
        collission_map[i].push_back(j);
        collided_indices.push_back(j);
      }
    }
  }

  // Now again loop over the updates and only push back in the new updates if it will not be merged into an other entity
  for (int i = 0; i < updates.size(); ++i)
  {
    // If index in collided_indices, it will be merged to another one
    if (std::find(collided_indices.begin(), collided_indices.end(), i) == collided_indices.end())
    {
      // Merging is done by creating a new convexHull. Multiple objects can be merged into one.
      // No sorting is done. So depending on which entity was taken first, that one is used as basis for merging.
      // But that shouldn't matter, only for UUID. Although the entities are re-associatied afterwards.
      // Which match new measurments to old entities.

      EntityUpdate u1 = updates[i];
      for (std::vector<int>::iterator it = collission_map[i].begin(); it != collission_map[i].end(); ++it)
      {
          ROS_DEBUG_COND(it == collission_map[i].begin(), "Merging entity %i and xx", i);
          ROS_DEBUG("Merging entity %i and %i", i, *it);
          const EntityUpdate u2 = updates[*it];
          u1 = mergeConvexHulls(image, sensor_pose, cam_model, segmenter_, u1, u2);

      }
      new_updates.push_back(u1);
      ROS_DEBUG("Adding update of entity %i", i);
    }
    else
    {
        ROS_DEBUG("Skipping update %i because of collision", i);
    }
  }

  return new_updates;
}

// ----------------------------------------------------------------------------------------------------

Updater::Updater()
{
}

// ----------------------------------------------------------------------------------------------------

Updater::~Updater()
{
}

// ----------------------------------------------------------------------------------------------------


bool Updater::update(const ed::WorldModel& world, const rgbd::ImageConstPtr& image, const geo::Pose3D& sensor_pose_const,
                     const UpdateRequest& req, UpdateResult& res, bool apply_roi)
{
    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // Prepare some things

    // will contain depth image filtered with given update shape and world model (background) subtraction
    cv::Mat filtered_depth_image;

    // sensor pose might be update, so copy (making non-const)
    geo::Pose3D sensor_pose = sensor_pose_const;

    // depth image
    const cv::Mat& depth = image->getDepthImage();

    // Determine depth image camera model
    rgbd::View view(*image, depth.cols);
    const geo::DepthCamera& cam_model = view.getRasterizer();

    std::string area_description;

    if (!req.area_description.empty())
    {
        // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        // Check if the update_command is a segmented entity.
        // If so, lookup the corresponding area_description

        bool fit_supporting_entity = true;

        std::map<ed::UUID, std::string>::const_iterator it_area_descr = id_to_area_description_.find(req.area_description);
        if (it_area_descr != id_to_area_description_.end())
        {
            area_description = it_area_descr->second;
            fit_supporting_entity = false; // We are only interested in the supported entity, so don't fit the supporting entity
        }
        else
            area_description = req.area_description;

        // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        // Parse space description (split on space)

        std::size_t i_space = area_description.find(' ');

        ed::UUID entity_id;
        std::string area_name;

        if (i_space == std::string::npos)
        {
            entity_id = area_description;
        }
        else
        {
            area_name = area_description.substr(0, i_space);
            entity_id = area_description.substr(i_space + 1);
        }

        // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        // Check for entity

        ed::EntityConstPtr e = world.getEntity(entity_id);

        if (!e)
        {
            res.error << "No such entity: '" << entity_id.str() << "'.";
            return false;
        }
        else if (!e->has_pose())
        {
            res.error << "Entity: '" << entity_id.str() << "' has no pose.";
            return false;
        }

        // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        // Update entity position

        geo::Pose3D new_pose;

        if (fit_supporting_entity)
        {
            FitterData fitter_data;
            if(apply_roi && e->has_roi())
            {
                //The ROI values are local to the object description in the YAML (no world context).
                //Add the height of the object in the world model
                geo::Pose3D pose = e->pose();
                float min = e->ROI()->min + pose.t.z;
                float max = e->ROI()->max + pose.t.z;

                fitter_.processSensorData(*image, sensor_pose, fitter_data, e->ROI()->include, min, max);
            }
            else
            {
                fitter_.processSensorData(*image, sensor_pose, fitter_data);
            }

            if (fitter_.estimateEntityPose(fitter_data, world, entity_id, e->pose(), new_pose, req.max_yaw_change, apply_roi))
            {
                bool hasStateUpdateGroup = !e->stateUpdateGroup().empty();
                if(apply_roi && hasStateUpdateGroup)
                {
                    if(e->hasFlag("state-update-group-main"))
                    {
                        updateStateGroupPose(world, res, e, new_pose);
                    }
                    else if(e->has_move_restrictions())
                    {
                        updateRestricted(res, e, new_pose);
                    }
                    else
                    {
                        res.update_req.setPose(entity_id, new_pose);
                    }
                }
                else
                {
                    res.update_req.setPose(entity_id, new_pose);
                }
            }
            else
            {
//                res.error << "Could not determine pose of '" << entity_id.str() << "'.";
//                return false;

                // Could not fit entity, so keep the old pose
                new_pose = e->pose();
            }
        }
        else
        {
            new_pose = e->pose();
        }

        // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        // Optimize sensor pose

        if (false)
        {
            fitZRP(*e->shape(), new_pose, *image, sensor_pose_const, sensor_pose);

            ROS_DEBUG_STREAM("Old sensor pose: " << sensor_pose_const);
            ROS_DEBUG_STREAM("New sensor pose: " << sensor_pose);
        }

        // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
        // Determine segmentation area

        if (!area_name.empty())
        {
            // Determine segmentation area (the geometrical shape in which the segmentation should take place)

            geo::Shape shape;
            bool found = false;
            tue::config::Reader r(e->data());

            if (r.readArray("areas"))
            {
                while(r.nextArrayItem())
                {
                    std::string a_name;
                    if (!r.value("name", a_name) || a_name != area_name)
                        continue;

                    if (ed::deserialize(r, "shape", shape))
                    {
                        found = true;
                        break;
                    }
                }

                r.endArray();
            }

            if (!found)
            {
                res.error << "No area '" << area_name << "' for entity '" << entity_id.str() << "'.";
                return false;
            }
            else if (shape.getMesh().getTriangleIs().empty())
            {
                res.error << "Could not load shape of area '" << area_name << "' for entity '" << entity_id.str() << "'.";
                return false;
            }

            // - - - - - - - - - - - - - - - - - - - - - - - -
            // Segment

            geo::Pose3D shape_pose = sensor_pose.inverse() * new_pose;
            segmenter_.calculatePointsWithin(*image, shape, shape_pose, filtered_depth_image);
        }
    }
    else
    {
        filtered_depth_image = image->getDepthImage().clone();
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // Remove background

    // The world model may have been updated above, but the changes are only captured in
    // an update request. Therefore, make a (shallow) copy of the world model and apply
    // the changes, and use this for the background removal

    ed::WorldModel world_updated = world;
    world_updated.update(res.update_req);

    segmenter_.removeBackground(filtered_depth_image, world_updated, cam_model, sensor_pose, req.background_padding);

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    // Clear convex hulls that are no longer there

    std::vector<ed::EntityConstPtr> associatable_entities;
    for(ed::WorldModel::const_iterator e_it = world.begin(); e_it != world.end(); ++e_it)
    {
        const ed::EntityConstPtr& e = *e_it;
        if (e->shape() || !e->has_pose() || e->convexHull().points.empty())
            continue;

        associatable_entities.push_back(e);
        /*
        geo::Vec3 p_3d = sensor_pose.inverse() * e->pose().t;

        cv::Point p_2d = cam_model.project3Dto2D(p_3d);
        if (p_2d.x < 0 || p_2d.y < 0 || p_2d.x >= depth.cols || p_2d.y >= depth.rows)
            continue;
        */
        /*float d = depth.at<float>(p_2d);
        if (d > 0 && d == d && -p_3d.z < d)
        {
            ROS_INFO("Request to remove entity %s", e->id().c_str());
            res.update_req.removeEntity(e->id());
            associatable_entities.pop_back();
        }*/
    }

//    cv::imshow("depth image", depth / 10);
//    cv::imshow("segments", filtered_depth_image / 10);
//    cv::waitKey();

    // - - - - - - - - - - - - - - - - - - - - - - - -
    // Cluster
    segmenter_.cluster(filtered_depth_image, cam_model, sensor_pose, res.entity_updates);

    // - - - - - - - - - - - - - - - - - - - - - - - -
    // Merge the detected clusters if they overlap in XY or Z
    res.entity_updates = mergeOverlappingConvexHulls(*image, sensor_pose, cam_model, segmenter_, res.entity_updates);

    // - - - - - - - - - - - - - - - - - - - - - - - -
    // Increase the convex hulls a bit towards the supporting surface and re-calculate mask
    // Then shrink the convex hulls again to get rid of the surface pixels

    for(std::vector<EntityUpdate>::iterator it = res.entity_updates.begin(); it != res.entity_updates.end(); ++it)
    {
        EntityUpdate& up = *it;

        up.chull.z_min -= 0.04;
        refitConvexHull(*image, sensor_pose, cam_model, segmenter_, up);

        up.chull.z_min += 0.01;
        refitConvexHull(*image, sensor_pose, cam_model, segmenter_, up);
    }

    // - - - - - - - - - - - - - - - - - - - - - - - -
    // Perform association and update
    associateAndUpdate(associatable_entities, image, sensor_pose, res.entity_updates, res.update_req);

    // - - - - - - - - - - - - -  - - - - - - - -  - - -
    // Remove entities that are not associated
    for (std::vector<ed::EntityConstPtr>::const_iterator it = associatable_entities.begin(); it != associatable_entities.end(); ++it)
    {
        ed::EntityConstPtr e = *it;

        // Check if entity is in frustum
        geo::Vec3 p_3d = sensor_pose.inverse() * e->pose().t;
        cv::Point p_2d = cam_model.project3Dto2D(p_3d);
        if (p_2d.x < 0 || p_2d.y < 0 || p_2d.x >= depth.cols || p_2d.y >= depth.rows)
            continue;

        // If the entity is not updated, remove it
        if (res.update_req.updated_entities.find(e->id()) == res.update_req.updated_entities.end())
        {
            ROS_INFO("Entity not associated and not found in frustum");

            float d = depth.at<float>(p_2d);
            if (d > 0 && d == d && -p_3d.z < d)
            {
                ROS_INFO("We can shoot a ray through the center, removing entity %s", e->id().c_str());
                res.update_req.removeEntity(e->id());
            }
        }

    }

    // - - - - - - - - - - - - - - - - - - - - - - - -
    // Remember the area description with which the segments where found

    if (!area_description.empty())
    {
       for(std::vector<EntityUpdate>::const_iterator it = res.entity_updates.begin(); it != res.entity_updates.end(); ++it)
        {
            const EntityUpdate& up = *it;
            id_to_area_description_[up.id] = area_description;
        }
    }

    return true;
}


void Updater::updateStateGroupPose(const ed::WorldModel& world, const UpdateResult& res, const ed::EntityConstPtr& mainObject, const geo::Pose3D& new_pose)
{
    std::string mainGroupName = mainObject->stateUpdateGroup();

    // use AngleBetweenTwoQuaternions instead of geo::Transform3::getYaw, because getYaw behaves strangely
    const double yawDif = ed_sensor_integration::math_helper::AngleBetweenTwoQuaternions(mainObject->pose().getQuaternion(), new_pose.getQuaternion());
    double c = cos(yawDif);
    double s = sin(yawDif);
    geo::Mat3 yawRotMat = geo::Mat3(c, -s, 0, s, c, 0, 0, 0, 1);
    geo::Vec3 pose_dif = new_pose.getOrigin() - mainObject->pose().getOrigin();

    for(ed::WorldModel::const_iterator it = world.begin(); it != world.end(); ++it)
    {
        const ed::EntityConstPtr& e = *it;
        if(mainGroupName.compare(e->stateUpdateGroup()) == 0)
        {
            geo::Pose3D new_pose;

            geo::Vec3 new_sub_position = e->pose().getOrigin() - mainObject->pose().getOrigin();
            new_sub_position = yawRotMat * new_sub_position;
            new_sub_position += mainObject->pose().getOrigin();

            new_pose.setOrigin(new_sub_position + pose_dif);
            new_pose.setBasis(e->pose().getBasis() * yawRotMat);

            res.update_req.setPose(e->id(), new_pose);
        }
    }
}

void Updater::updateRestricted(const UpdateResult& res, const ed::EntityConstPtr& obj, geo::Pose3D& new_pose)
{
    if(obj->moveRestrictions()->canMove)
    {
        geo::Vec3 moveDir = geo::Vec3(obj->moveRestrictions()->moveDirection.x, obj->moveRestrictions()->moveDirection.y, 0);

        geo::Mat3 yawRotMat = ed_sensor_integration::math_helper::QuaternionToRotationMatrix(obj->pose().getQuaternion());
        geo::Mat3 yawRotMat2 = geo::Mat3(0, -1, 0,1, 0, 0, 0, 0, 1); // yawRotMat is 90° wrong, -+ doesnt matter

        moveDir = yawRotMat * yawRotMat2 * moveDir;
        geo::Vec3 moveDirNorm = moveDir.normalized();

        geo::Vec3 newPoseMoveDir = new_pose.getOrigin() - obj->pose().getOrigin();
        geo::Vec3 newPoseMoveDirNorm = newPoseMoveDir.normalized();

        double dotProd = moveDirNorm.dot(newPoseMoveDirNorm);
        dotProd *= newPoseMoveDir.length();

        new_pose = geo::Pose3D(new_pose.R, obj->pose().t + moveDirNorm * dotProd);
    }
    else
    {
        //Do not update position
        new_pose.setOrigin(obj->pose().t);
    }

    if(!obj->moveRestrictions()->canRotate)
    {
        //Do not update rotation
        new_pose.setBasis(obj->pose().R);
    }

    if(!obj->moveRestrictions()->canMove && !obj->moveRestrictions()->canRotate)
    {
        ROS_WARN("Updater::updateRestricted(..). Entity %s can not move or rotate. Might be an error in the YAML.", obj->type().c_str());
    }


    res.update_req.setPose(obj->id(), new_pose);
}
