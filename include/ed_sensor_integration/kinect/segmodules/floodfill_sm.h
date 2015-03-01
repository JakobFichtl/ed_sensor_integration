#ifndef ed_sensor_integration_kinect_floodfill_sm_h_
#define ed_sensor_integration_kinect_floodfill_sm_h_

#include "ed/segmentation_modules/rgbd_seg_module.h"

#include <opencv2/core/core.hpp>

namespace ed
{

class FloodfillSM : public RGBDSegModule
{

public:

    FloodfillSM();

    void process(const RGBDData& rgbd_data, std::vector<PointCloudMaskPtr>& segments);

private:

    void findBlobs(cv::Mat& depth_image, const ImageMask& mask, std::vector<ImageMask>& blobs);
    bool findBlob(cv::Mat& depth_image, const cv::Mat& mask, cv::Point p, int label, ImageMask& blob);


};

}

#endif
