#include <iostream>
#include <boost/python.hpp>
#include <Python.h>
#include <vector>


#include "common.h"
#include "opencv2/gpu/gpu.hpp"

#include "opencv2/video/tracking.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/calib3d/calib3d.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/features2d/features2d.hpp"
#include "opencv2/core/core.hpp"
#include "opencv2/nonfree/nonfree.hpp"
#include "opencv2/gpu/gpu.hpp"
#include "opencv2/gpu/gpu.hpp"


using namespace cv::gpu;
using namespace cv;

namespace bp = boost::python;

class TVL1FlowExtractor{
public:

    TVL1FlowExtractor(int bound){
        bound_ = bound;
    }

    static void set_device(int dev_id){
        setDevice(dev_id);
    }

    bp::list extract_flow(bp::list frames, int img_width, int img_height){
        bp::list output;
        Mat input_frame, prev_frame, next_frame, prev_gray, next_gray;
        Mat flow_x, flow_y;




        // initialize the first frame
        const char* first_data = ((const char*)bp::extract<const char*>(frames[0]));
        input_frame = Mat(img_height, img_width, CV_8UC3);
        initializeMats(input_frame, prev_frame, prev_gray, next_frame, next_gray);

        memcpy(prev_frame.data, first_data, bp::len(frames[0]));
        cvtColor(prev_frame, prev_gray, CV_BGR2GRAY);
        for (int idx = 1; idx < bp::len(frames); idx++){
            const char* this_data = ((const char*)bp::extract<const char*>(frames[idx]));
            memcpy(next_frame.data, this_data, bp::len(frames[0]));
            cvtColor(next_frame, next_gray, CV_BGR2GRAY);

            d_frame_0.upload(prev_gray);
            d_frame_1.upload(next_gray);

            alg_tvl1(d_frame_0, d_frame_1, d_flow_x, d_flow_y);

            d_flow_x.download(flow_x);
            d_flow_y.download(flow_y);

            vector<uchar> str_x, str_y;

            encodeFlowMap(flow_x, flow_y, str_x, str_y, bound_, false);
            output.append(
                bp::make_tuple(
                    bp::str((const char*) str_x.data(), str_x.size()),
                    bp::str((const char*) str_y.data(), str_y.size())
                    )
            );

            std::swap(prev_gray, next_gray);
        }
        return output;
    };
private:
    int bound_;
    GpuMat d_frame_0, d_frame_1;
    GpuMat d_flow_x, d_flow_y;
    OpticalFlowDual_TVL1_GPU alg_tvl1;
};

extern void MyWarpPerspective(Mat& prev_src, Mat& src, Mat& dst, Mat& M0, int flags = INTER_LINEAR,
                       int borderType = BORDER_CONSTANT, const Scalar& borderValue = Scalar());
extern void ComputeMatch(const std::vector<KeyPoint>& prev_kpts, const std::vector<KeyPoint>& kpts,
                  const Mat& prev_desc, const Mat& desc, std::vector<Point2f>& prev_pts, std::vector<Point2f>& pts);
extern void MergeMatch(const std::vector<Point2f>& prev_pts1, const std::vector<Point2f>& pts1,
                       const std::vector<Point2f>& prev_pts2, const std::vector<Point2f>& pts2,
                       std::vector<Point2f>& prev_pts_all, std::vector<Point2f>& pts_all);
extern void MatchFromFlow_copy(const Mat& prev_grey, const Mat& flow_x, const Mat& flow_y, std::vector<Point2f>& prev_pts, std::vector<Point2f>& pts, const Mat& mask);

class TVL1WarpFlowExtractor {
public:

    TVL1WarpFlowExtractor(int bound)
        :detector_surf(200), extractor_surf(true, true){
        bound_ = bound;
    }

    static void set_device(int dev_id){
        setDevice(dev_id);
    }

    bp::list extract_warp_flow(bp::list frames, int img_width, int img_height){
        bp::list output;
        Mat input_frame, prev_frame, next_frame, prev_gray, next_gray, human_mask;
        Mat flow_x, flow_y;

        // initialize the first frame
        const char* first_data = ((const char*)bp::extract<const char*>(frames[0]));
        input_frame = Mat(img_height, img_width, CV_8UC3);
        initializeMats(input_frame, prev_frame, prev_gray, next_frame, next_gray);
        human_mask = Mat::ones(input_frame.size(), CV_8UC1);

        memcpy(prev_frame.data, first_data, bp::len(frames[0]));
        cvtColor(prev_frame, prev_gray, CV_BGR2GRAY);
        for (int idx = 1; idx < bp::len(frames); idx++){
            const char* this_data = ((const char*)bp::extract<const char*>(frames[idx]));
            memcpy(next_frame.data, this_data, bp::len(frames[0]));
            cvtColor(next_frame, next_gray, CV_BGR2GRAY);

            d_frame_0.upload(prev_gray);
            d_frame_1.upload(next_gray);

            alg_tvl1(d_frame_0, d_frame_1, d_flow_x, d_flow_y);

            d_flow_x.download(flow_x);
            d_flow_y.download(flow_y);

            // warp to reduce holistic motion
            detector_surf.detect(next_gray, kpts_surf, human_mask);
            extractor_surf.compute(next_gray, kpts_surf, desc_surf);
            ComputeMatch(prev_kpts_surf, kpts_surf, prev_desc_surf, desc_surf, prev_pts_surf, pts_surf);
            MatchFromFlow_copy(next_gray, flow_x, flow_y, prev_pts_flow, pts_flow, human_mask);
            MergeMatch(prev_pts_flow, pts_flow, prev_pts_surf, pts_surf, prev_pts_all, pts_all);
            Mat H = Mat::eye(3, 3, CV_64FC1);
            if(pts_all.size() > 50) {
                std::vector<unsigned char> match_mask;
                Mat temp = findHomography(prev_pts_all, pts_all, RANSAC, 1, match_mask);
                if(countNonZero(Mat(match_mask)) > 25)
                    H = temp;
            }

            Mat H_inv = H.inv();
            Mat gray_warp = Mat::zeros(next_gray.size(), CV_8UC1);
            MyWarpPerspective(prev_gray, next_gray, gray_warp, H_inv);

            d_frame_0.upload(prev_gray);
            d_frame_1.upload(gray_warp);

            alg_tvl1(d_frame_0, d_frame_1, d_flow_x, d_flow_y);

            d_flow_x.download(flow_x);
            d_flow_y.download(flow_y);

            vector<uchar> str_x, str_y;

            encodeFlowMap(flow_x, flow_y, str_x, str_y, bound_, false);
            output.append(
                    bp::make_tuple(
                            bp::str((const char*) str_x.data(), str_x.size()),
                            bp::str((const char*) str_y.data(), str_y.size())
                    )
            );

            std::swap(prev_gray, next_gray);
        }
        return output;
    }
private:
    SurfFeatureDetector detector_surf;
    SurfDescriptorExtractor extractor_surf;
    std::vector<Point2f> prev_pts_flow, pts_flow;
    std::vector<Point2f> prev_pts_surf, pts_surf;
    std::vector<Point2f> prev_pts_all, pts_all;
    std::vector<KeyPoint> prev_kpts_surf, kpts_surf;
    Mat prev_desc_surf, desc_surf;

    GpuMat d_frame_0, d_frame_1;
    GpuMat d_flow_x, d_flow_y;

    OpticalFlowDual_TVL1_GPU alg_tvl1;
    int bound_;
};


//// Boost Python Related Decl
BOOST_PYTHON_MODULE(libpydenseflow){
    bp::class_<TVL1FlowExtractor>("TVL1FlowExtractor", bp::init<int>())
            .def("extract_flow", &TVL1FlowExtractor::extract_flow)
            .def("set_device", &TVL1FlowExtractor::set_device)
            .staticmethod("set_device");
    bp::class_<TVL1FlowExtractor>("TVL1WarpFlowExtractor", bp::init<int>())
            .def("extract_warp_flow", &TVL1WarpFlowExtractor::extract_warp_flow)
            .def("set_device", &TVL1WarpFlowExtractor::set_device)
            .staticmethod("set_device");
}