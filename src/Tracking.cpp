/*********
*   构造函数Tracking()构造Tracking对象，读取Setting参数。
*   GrabImageMonocular转换读取的帧为灰度，调用Track()
*   Track() 
**********/

#include "Tracking.h"

#include <opencv2/core/core.hpp>
#include <opencv2/features2d/features2d.hpp>

#include "ORBmatcher.h"
#include "FrameDrawer.h"
#include "Converter.h"
#include "Map.h"
#include "Initializer.h"

#include "Optimizer.h"
#include "PnPsolver.h"

#include <iostream>
#include <cmath>
#include <mutex>


#define TRACK_WITH_IMU

using namespace std;

// "m"表示类中的成员变量
// "p"表示指针数据类型
// "n"表示int类型
// "b"表示bool类型
// "s"表示set类型
// "v"表示vector数据类型
// 'l'表示list数据类型
// "KF"表示KeyPoint数据类型

namespace ORB_SLAM2
{



    /************************vSLAM*********************************/

    // 重定位成功后，通过新的20帧图像，计算当前帧的导航状态和bias
    void Tracking::RecomputeIMUBiasAndCurrentNavstate(NavState &nscur)
    {
        size_t N = mv20FramesReloc.size();

        // test log
        if (N != 20)
            cerr << "Frame vector size not 20 to compute bias after reloc??? size: " << mv20FramesReloc.size() << endl;

        // 估计gyr bias
        Vector3d bg = Optimizer::OptimizeInitialGyroBias(mv20FramesReloc);

        for (size_t i = 0; i < N; i++)
        {
            Frame &frame = mv20FramesReloc[i];

            // test log 重置之前应该是0
            if (frame.GetNavState().Get_BiasGyr().norm() != 0 || frame.GetNavState().Get_dBias_Gyr().norm() != 0)
                cerr << "Frame " << frame.mnId << " gyr bias or delta bias not zero???" << endl;

            // 重置陀螺bias
            frame.SetNavStateBiasGyr(bg);
        }

        // 计算帧图像间的预积分
        vector<IMUPreintegrator> v19IMUPreint;
        v19IMUPreint.reserve(19);
        for (size_t i = 0; i < N; i++)
        {
            if (i == 0)
                continue;

            const Frame &Fi = mv20FramesReloc[i - 1];
            const Frame &Fj = mv20FramesReloc[i];

            IMUPreintegrator imupreint;
            Fj.ComputeIMUPreIntSinceLastFrame(&Fi, imupreint);
            v19IMUPreint.push_back(imupreint);
        }

        // 求解加速度计bias
        // 构建方程 [A1;A2;...;AN]*ba = [B1;B2;../BN]
        cv::Mat A = cv::Mat::zeros(3 * (N - 2), 3, CV_32F);
        cv::Mat B = cv::Mat::zeros(3 * (N - 2), 1, CV_32F);

        const cv::Mat &gw = mpLocalMapper->GetGravityVec();
        const cv::Mat &Tcb = ConfigParam::GetMatT_cb();

        for (size_t i = 0; i < N - 2; i++)
        {
            const Frame &F1 = mv20FramesReloc[i];
            const Frame &F2 = mv20FramesReloc[i + 1];
            const Frame &F3 = mv20FramesReloc[i + 2];

            const IMUPreintegrator &PreInt12 = v19IMUPreint[i];
            const IMUPreintegrator &PreInt23 = v19IMUPreint[i + 1];

            // 预积分IMU数据时间差
            double dt12 = PreInt12.getDeltaTime();
            double dt23 = PreInt23.getDeltaTime();

            // 预积分测量值
            cv::Mat dp12 = Converter::toCvMat(PreInt12.getDeltaP());
            cv::Mat dv12 = Converter::toCvMat(PreInt12.getDeltaV());
            cv::Mat dp23 = Converter::toCvMat(PreInt23.getDeltaP());
            cv::Mat Jpba12 = Converter::toCvMat(PreInt12.getJPBiasa());
            cv::Mat Jvba12 = Converter::toCvMat(PreInt12.getJVBiasa());
            cv::Mat Jpba23 = Converter::toCvMat(PreInt23.getJPBiasa());

            // 导航坐标系在世界坐标系位姿
            cv::Mat Twb1 = Converter::toCvMatInverse(F1.mTcw) * Tcb;
            cv::Mat Twb2 = Converter::toCvMatInverse(F2.mTcw) * Tcb;
            cv::Mat Twb3 = Converter::toCvMatInverse(F3.mTcw) * Tcb;

            // 导航坐标系在世界坐标系位置
            cv::Mat pb1 = Twb1.rowRange(0, 3).col(3);
            cv::Mat pb2 = Twb2.rowRange(0, 3).col(3);
            cv::Mat pb3 = Twb3.rowRange(0, 3).col(3);

            // 导航坐标系在世界坐标系姿态
            cv::Mat Rb1 = Twb1.rowRange(0, 3).colRange(0, 3);
            cv::Mat Rb2 = Twb2.rowRange(0, 3).colRange(0, 3);

            // VI ORB论文公式13，20变形，因为是重定位，尺度可以认为是正确的, s=1
            cv::Mat Ai = Rb1 * Jpba12 * dt23 - Rb2 * Jpba23 * dt12 - Rb1 * Jvba12 * dt12 * dt23;
            cv::Mat Bi = (pb2 - pb3) * dt12 + (pb2 - pb1) * dt23 + Rb2 * dp23 * dt12 - Rb1 * dp12 * dt23 +
                         Rb1 * dv12 * dt12 * dt23 + 0.5 * gw * (dt12 * dt12 * dt23 + dt12 * dt23 * dt23);
            Ai.copyTo(A.rowRange(3 * i + 0, 3 * i + 3));
            Bi.copyTo(B.rowRange(3 * i + 0, 3 * i + 3));

            if (fabs(F2.mTimeStamp - F1.mTimeStamp - dt12) > 1e-6 || fabs(F3.mTimeStamp - F2.mTimeStamp - dt23) > 1e-6)
                cerr << "delta time not right." << endl;

        }

        // 使用SVD计算 A*x=B, x=ba 3x1 vector
        // A = u*w*vt, u*w*vt*x=B
        // x = vt'*winv*u'*B
        cv::Mat w2, u2, vt2;
        cv::SVDecomp(A, w2, u2, vt2, cv::SVD::MODIFY_A);

        cv::Mat w2inv = cv::Mat::eye(3, 3, CV_32F);
        for (int i = 0; i < 3; i++)
        {
            if (fabs(w2.at<float>(i)) < 1e-10)
            {
                w2.at<float>(i) += 1e-10;
                cerr << "w2(i) < 1e-10, w=" << endl << w2 << endl;
            }

            w2inv.at<float>(i, i) = 1. / w2.at<float>(i);
        }

        cv::Mat ba_cv = vt2.t() * w2inv * u2.t() * B;
        Vector3d ba = Converter::toVector3d(ba_cv);

        // 更新Acc Bias
        for (size_t i = 0; i < N; i++)
        {
            Frame &frame = mv20FramesReloc[i];

            if (frame.GetNavState().Get_BiasAcc().norm() != 0 || frame.GetNavState().Get_dBias_Gyr().norm() != 0 ||
                frame.GetNavState().Get_dBias_Acc().norm() != 0)
                cerr << "Frame " << frame.mnId << " acc bias or delta bias not zero???" << endl;

            frame.SetNavStateBiasAcc(ba);
        }

        // 估计当前帧的速度
        Vector3d Pcur;
        Vector3d Vcur;
        Matrix3d Rcur;
        {
            Frame &F1 = mv20FramesReloc[N - 2];
            Frame &F2 = mv20FramesReloc[N - 1];

            const IMUPreintegrator &imupreint = v19IMUPreint.back();
            const double dt12 = imupreint.getDeltaTime();
            const Vector3d dp12 = imupreint.getDeltaP();
            const Vector3d gweig = Converter::toVector3d(gw);
            const Matrix3d Jpba12 = imupreint.getJPBiasa();
            const Vector3d dv12 = imupreint.getDeltaV();
            const Matrix3d Jvba12 = imupreint.getJVBiasa();

            // 估计前一帧的速度
            // VI ORB公式18
            // P2 = P1 + V1*dt12 + 0.5*gw*dt12*dt12 + R1*(dP12 + Jpba*ba + Jpbg*0)
            cv::Mat Twb1 = Converter::toCvMatInverse(F1.mTcw) * Tcb;
            cv::Mat Twb2 = Converter::toCvMatInverse(F2.mTcw) * Tcb;
            Vector3d P1 = Converter::toVector3d(Twb1.rowRange(0, 3).col(3));
            Pcur = Converter::toVector3d(Twb2.rowRange(0, 3).col(3));
            Matrix3d R1 = Converter::toMatrix3d(Twb1.rowRange(0, 3).colRange(0, 3));
            Rcur = Converter::toMatrix3d(Twb2.rowRange(0, 3).colRange(0, 3));
            Vector3d V1 = 1. / dt12 * (Pcur - P1 - 0.5 * gweig * dt12 * dt12 - R1 * (dp12 + Jpba12 * ba));

            // 当前帧速度
            Vcur = V1 + gweig * dt12 + R1 * (dv12 + Jvba12 * ba);

            // test log
            if (F2.mnId != mCurrentFrame.mnId)
                cerr << "framecur.mnId != mCurrentFrame.mnId. why??" << endl;
            if (fabs(F2.mTimeStamp - F1.mTimeStamp - dt12) > 1e-6)
                cerr << "timestamp not right?? in compute vel" << endl;
        }

        // 设置CF的导航状态
        nscur.Set_Pos(Pcur);
        nscur.Set_Vel(Vcur);
        nscur.Set_Rot(Rcur);
        nscur.Set_BiasGyr(bg);
        nscur.Set_BiasAcc(ba);
        nscur.Set_DeltaBiasGyr(Vector3d::Zero());
        nscur.Set_DeltaBiasAcc(Vector3d::Zero());

    }


    // 跟踪局部地图，利用IMU信息加入运动方程约束
    bool Tracking::TrackLocalMapWithIMU(bool bMapUpdated)
    {

        // 提取局部地图
        UpdateLocalMap();

        // 局部地图与当前帧匹配
        SearchLocalPoints();

        // 判断VI SLAM初始化完成或Map是否更新，是优化KF位姿
        if (mpLocalMapper->GetFirstVINSInited() || bMapUpdated)
        {
            // 获取上一帧关键帧到当前帧的IMU预积分
            IMUPreintegrator imupreint = GetIMUPreIntSinceLastKF(&mCurrentFrame, mpLastKeyFrame, mvIMUSinceLastKF);

            // test log
            if (mCurrentFrame.GetNavState().Get_dBias_Acc().norm() > 1e-6)
                cerr << "TrackLocalMapWithIMU current Frame dBias acc not zero" << endl;

            if (mCurrentFrame.GetNavState().Get_dBias_Gyr().norm() > 1e-6)
                cerr << "TrackLocalMapWithIMU current Frame dBias gyr not zero" << endl;

            Optimizer::PoseOptimization(&mCurrentFrame, mpLastKeyFrame, imupreint, mpLocalMapper->GetGravityVec(),
                                        true);
        }
            // 地图没有更新，和上一帧优化
            // 运动方程和帧的相机位姿无关，所以无论是否完成初始化都可进行求解。
        else
        {
            IMUPreintegrator imupreint = GetIMUPreIntSinceLastFrame(&mCurrentFrame, &mLastFrame);
            Optimizer::PoseOptimization(&mCurrentFrame, &mLastFrame, imupreint, mpLocalMapper->GetGravityVec(), true);
        }

        mnMatchesInliers = 0;
        // 更新CF的地图点云属性，记录内点数量。
        for (int i = 0; i < mCurrentFrame.N; i++)
        {
            if (mCurrentFrame.mvpMapPoints[i])
            {
                if (!mCurrentFrame.mvbOutlier[i])
                {
                    mCurrentFrame.mvpMapPoints[i]->IncreaseFound();

                    if (!mbOnlyTracking)
                    {
                        if (mCurrentFrame.mvpMapPoints[i]->Observations() > 0)
                            mnMatchesInliers++;
                    }
                    else
                        mnMatchesInliers++;
                }
                else if (mSensor == System::STEREO)
                    mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint *>(NULL);

            }
        }

        if (mCurrentFrame.mnId < mnLastRelocFrameId + mMaxFrames && mnMatchesInliers < 50)
            return false;

        if (mnMatchesInliers < 6 /*30*/)
            return false;

        else
            return true;

    }


    // 根据IMU估计CF导航状态，在TrackwithIMU中使用
    void Tracking::PredictNavStateByIMU(bool bMapUpdated)
    {
        if (!mpLocalMapper->GetVINSInited())
            cerr << "mpLocalMapper->GetVINSInited() not, shouldn't in PredictNavStateByIMU" << endl;

        // 地图更新，lastKF优化
        if (mpLocalMapper->GetFirstVINSInited() || bMapUpdated)
        {
            // 计算上一帧关键帧到当前帧的预积分
            mIMUPreIntInTrack = GetIMUPreIntSinceLastKF(&mCurrentFrame, mpLastKeyFrame, mvIMUSinceLastKF);

            mCurrentFrame.SetInitialNavStateAndBias(mpLastKeyFrame->GetNavState());
            mCurrentFrame.UpdateNavState(mIMUPreIntInTrack, Converter::toVector3d(mpLocalMapper->GetGravityVec()));
            mCurrentFrame.UpdatePoseFromNS(ConfigParam::GetMatTbc());

            // test log
            if (mCurrentFrame.GetNavState().Get_dBias_Acc().norm() > 1e-6)
                cerr << "PredictNavStateByIMU1 current Frame dBias acc not zero" << endl;
            if (mCurrentFrame.GetNavState().Get_dBias_Gyr().norm() > 1e-6)
                cerr << "PredictNavStateByIMU1 current Frame dBias gyr not zero" << endl;

        }
        else
        {
            // 计算上一帧到当前帧的预积分
            mIMUPreIntInTrack = GetIMUPreIntSinceLastFrame(&mCurrentFrame, &mLastFrame);

            // 从上一帧关键帧获取初始状态
            mCurrentFrame.SetInitialNavStateAndBias(mLastFrame.GetNavState());
            mCurrentFrame.UpdateNavState(mIMUPreIntInTrack, Converter::toVector3d(mpLocalMapper->GetGravityVec()));
            mCurrentFrame.UpdatePoseFromNS(ConfigParam::GetMatTbc());

            // test log
            if (mCurrentFrame.GetNavState().Get_dBias_Acc().norm() > 1e-6)
                cerr << "PredictNavStateByIMU2 current Frame dBias acc not zero" << endl;
            if (mCurrentFrame.GetNavState().Get_dBias_Gyr().norm() > 1e-6)
                cerr << "PredictNavStateByIMU2 current Frame dBias gyr not zero" << endl;

        }

    }


    // 通过IMU更新的导航状态进行跟踪匹配
    bool Tracking::TrackWithIMU(bool bMapUpdated)
    {
        ORBmatcher matcher(0.9, true);

        // IMU初始化后进行Track
        if (!mpLocalMapper->GetVINSInited())
            cerr << "local mapping VINS not inited. why call TrackWithIMU?" << endl;

        // 更新当前帧的导航状态，相当于IMU提供运动模型
        PredictNavStateByIMU(bMapUpdated);

        // 初始化当前帧所有地图点为空
        fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint *>(NULL));

        int th;
        if (mSensor != System::STEREO)
            th = 15;
        else
            th = 7;

        int nmatches = matcher.SearchByProjection(mCurrentFrame, mLastFrame, th, mSensor == System::MONOCULAR);

        // 匹配太少，使用更宽的窗口匹配
        if (nmatches < 20)
        {
            fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint *>(NULL));
            nmatches = matcher.SearchByProjection(mCurrentFrame, mLastFrame, 2 * th, mSensor == System::MONOCULAR);
        }

        if (nmatches < /*20*/10)
            return false;

        // VI初始化完成来了与KF建立运动约束算位姿，否则与上一帧建立运动约束计算位姿
        if (mpLocalMapper->GetFirstVINSInited() || bMapUpdated)
        {
            Optimizer::PoseOptimization(&mCurrentFrame, mpLastKeyFrame, mIMUPreIntInTrack,
                                        mpLocalMapper->GetGravityVec(), false);
        }
        else
        {
            Optimizer::PoseOptimization(&mCurrentFrame, &mLastFrame, mIMUPreIntInTrack, mpLocalMapper->GetGravityVec(),
                                        false);
        }


        int nmatchesMap = 0;
        for (int i = 0; i < mCurrentFrame.N; i++)
        {
            if (mCurrentFrame.mvpMapPoints[i])
            {
                if (mCurrentFrame.mvbOutlier[i])
                {
                    MapPoint *pMP = mCurrentFrame.mvpMapPoints[i];

                    mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint *>(NULL);
                    mCurrentFrame.mvbOutlier[i] = false;
                    pMP->mbTrackInView = false;
                    pMP->mnLastFrameSeen = mCurrentFrame.mnId;
                    nmatches--;
                }
                else if (mCurrentFrame.mvpMapPoints[i]->Observations() > 0)
                    nmatchesMap++;
            }

        }

        if (mbOnlyTracking)
        {
            mbVO = nmatchesMap < 10;
            return nmatches > 20;
        }

        return nmatchesMap >= /*10*/ 6;

    }


    // CF-LastKF IMU预积分
    IMUPreintegrator
    Tracking::GetIMUPreIntSinceLastKF(Frame *pCurF, KeyFrame *pLastKF, const std::vector<IMUData> &vIMUSInceLastKF)
    {
        // 重置IMU预积分
        IMUPreintegrator IMUPreInt;
        IMUPreInt.reset();

        Vector3d bg = pLastKF->GetNavState().Get_BiasGyr();
        Vector3d ba = pLastKF->GetNavState().Get_BiasAcc();

        // 时间戳从LastKF到第一个IMU数据的预积分
        {
            const IMUData &imu = vIMUSInceLastKF.front();
            double dt = imu._t - pLastKF->mTimeStamp;
            IMUPreInt.update(imu._g - bg, imu._a - ba, dt);

            // test log
            if (dt < 0)
            {
                cerr << std::fixed << std::setprecision(3) << "dt = " << dt << ", this KF vs last imu time: "
                     << pLastKF->mTimeStamp << " vs " << imu._t << endl;
                std::cerr.unsetf(std::ios::showbase);                // deactivate showbase
            }

        }

        for (size_t i = 0; i < vIMUSInceLastKF.size(); i++)
        {
            const IMUData &imu = vIMUSInceLastKF[i];
            double nextt;
            if (i == vIMUSInceLastKF.size() - 1)
                nextt = pCurF->mTimeStamp;
            else
                nextt = vIMUSInceLastKF[i + 1]._t;

            double dt = nextt - imu._t;

            // 预积分
            IMUPreInt.update(imu._g - bg, imu._a - ba, dt);


            // test log
            if (dt <= 0)
            {
                cerr << std::fixed << std::setprecision(3) << "dt = " << dt << ", this vs next time: " << imu._t
                     << " vs " << nextt << endl;
                std::cerr.unsetf(std::ios::showbase);                // deactivate showbase
            }

        }

        return IMUPreInt;

    }


    // LastF-CKF IMU预积分
    IMUPreintegrator Tracking::GetIMUPreIntSinceLastFrame(Frame *pCurF, Frame *pLastF)
    {
        // 重置IMU预积分值
        IMUPreintegrator IMUPreInt;
        IMUPreInt.reset();

        pCurF->ComputeIMUPreIntSinceLastFrame(pLastF, IMUPreInt);

        return IMUPreInt;
    }


    // VI Tracking线程入口
    cv::Mat Tracking::GrabImageMonoVI(const cv::Mat &im, const std::vector<IMUData> &vimu, const double &timestamp)
    {
        // 保存上一帧关键帧到当前帧的IMU数据
        mvIMUSinceLastKF.insert(mvIMUSinceLastKF.end(), vimu.begin(), vimu.end());
        mImGray = im;

        if (mImGray.channels() == 3)
        {
            if (mbRGB)
                cvtColor(mImGray, mImGray, CV_RGB2GRAY);
            else
                cvtColor(mImGray, mImGray, CV_BGR2GRAY);
        }
        else if (mImGray.channels() == 4)
        {
            if (mbRGB)
                cvtColor(mImGray, mImGray, CV_RGBA2GRAY);
            else
                cvtColor(mImGray, mImGray, CV_BGRA2GRAY);
        }

        if (mState == NOT_INITIALIZED || mState == NO_IMAGES_YET)
            mCurrentFrame = Frame(mImGray, timestamp, vimu, mpIniORBextractor, mpORBVocabulary, mK, mDistCoef, mbf,
                                  mThDepth);
        else
            mCurrentFrame = Frame(mImGray, timestamp, vimu, mpORBextractorLeft, mpORBVocabulary, mK, mDistCoef, mbf,
                                  mThDepth, mpLastKeyFrame);

        Track();

        return mCurrentFrame.mTcw.clone();

    }



    /**********************************************************/

    // 构造函数,初始化类型，传递参数。
    Tracking::Tracking(System *pSys, ORBVocabulary *pVoc, FrameDrawer *pFrameDrawer, MapDrawer *pMapDrawer, Map *pMap,
                       KeyFrameDatabase *pKFDB,
                       const string &strSettingPath, const int sensor, ConfigParam *pParams) :
            mState(NO_IMAGES_YET), mSensor(sensor), mbOnlyTracking(false), mbVO(false), mpORBVocabulary(pVoc),
            mpKeyFrameDB(pKFDB), mpInitializer(static_cast<Initializer *>(NULL)), mpSystem(pSys),
            mpFrameDrawer(pFrameDrawer), mpMapDrawer(pMapDrawer), mpMap(pMap), mnLastRelocFrameId(0)
    {
        mbCreateNewKFAfterReloc = false;
        mbRelocBiasPrepare = false;
        mpParams = pParams;

        // 加载相机标定参数。
        cv::FileStorage fSettings(strSettingPath, cv::FileStorage::READ);
        float fx = fSettings["Camera.fx"];
        float fy = fSettings["Camera.fy"];
        float cx = fSettings["Camera.cx"];
        float cy = fSettings["Camera.cy"];

        //     / fx  0   cx /
        // K = / 0   fy  cy /
        //     / 0   0    1 /
        cv::Mat K = cv::Mat::eye(3, 3, CV_32F);
        K.at<float>(0, 0) = fx;
        K.at<float>(1, 1) = fy;
        K.at<float>(0, 2) = cx;
        K.at<float>(1, 2) = cy;
        K.copyTo(mK);           // 复制给类成员变量。

        // 图像校正系数。
        // [k1,k2,p1,p2,k3]。
        cv::Mat DistCoef(4, 1, CV_32F);
        DistCoef.at<float>(0) = fSettings["Camera.k1"];
        DistCoef.at<float>(1) = fSettings["Camera.k2"];
        DistCoef.at<float>(2) = fSettings["Camera.p1"];
        DistCoef.at<float>(3) = fSettings["Camera.p2"];
        const float k3 = fSettings["Camera.k3"];
        if (k3 != 0)
        {
            DistCoef.resize(5);
            DistCoef.at<float>(4) = k3;
        }
        DistCoef.copyTo(mDistCoef);

        // bf= 双目基线*fx。
        mbf = fSettings["Camera.bf"];

        float fps = fSettings["Camera.fps"];
        if (fps == 0)
            fps = 30;

        // 插入关键帧和重定位的最大/最小帧。
        mMinFrames = 0;
        mMaxFrames = fps;

        // 输出相机参数，帧率，图像通道格式。
        cout << endl << "Camera Parameters: " << endl;
        cout << "- fx:" << fx << endl;
        cout << "- fy:" << fy << endl;
        cout << "- cx:" << cx << endl;
        cout << "- cy:" << cy << endl;
        cout << "- k1:" << DistCoef.at<float>(0) << endl;
        cout << "- k2:" << DistCoef.at<float>(1) << endl;
        if (DistCoef.rows == 5)
            cout << "- k3:" << DistCoef.at<float>(4) << endl;
        cout << "- p1:" << DistCoef.at<float>(2) << endl;
        cout << "- p2:" << DistCoef.at<float>(3) << endl;
        cout << "- fps:" << fps << endl;

        // 图片通道顺序 1:RGB  0:BGR。
        int nRGB = fSettings["Camera.RGB"];
        mbRGB = nRGB;
        if (mbRGB)
            cout << "- color order: RGB" << endl;
        else
            cout << "- color order: BGR" << endl;

        // 读取ORB参数。

        // 每帧提取的特征点数。
        int nFeatures = fSettings["ORBextractor.nFeatures"];
        // 图像建立金字塔时变化的尺度为 1.2。
        float fScaleFactor = fSettings["ORBextractor.scaleFactor"];
        // 尺度金字塔的层数 8。
        int nLevels = fSettings["ORBextractor.nLevels"];
        // 提取fast特征点的默认阈值 20。
        int fIniThFAST = fSettings["ORBextractor.iniThFAST"];
        // 默认阈值无法提取足够的fast特征点，使用最小阈值8。
        int fMinThFAST = fSettings["ORBextractor.minThFAST"];

        // 构造了一个ORBextractor类型的对象，跟踪过程都会用到mpORBextractorLeft作为特征点提取器，设置提取orb特征的性质。
        mpORBextractorLeft = new ORBextractor(nFeatures, fScaleFactor, nLevels, fIniThFAST, fMinThFAST);
        // 双目时，Tracking过程还会构造一个mpORBextractorRight做特征点提取器。
        if (sensor == System::STEREO)
            mpORBextractorRight = new ORBextractor(nFeatures, fScaleFactor, nLevels, fIniThFAST, fMinThFAST);

        // 单目时，会用mpIniORBextractor做特征点提取器,采用双倍特征点，获取更丰富信息，准确初始化。
        if (sensor == System::MONOCULAR)
            mpIniORBextractor = new ORBextractor(2 * nFeatures, fScaleFactor, nLevels, fIniThFAST, fMinThFAST);

        // 输出ORB特征点提取信息。
        cout << endl << "ORB Extractor Parameters: " << endl;
        cout << "- Number of Features: " << nFeatures << endl;
        cout << "- Scale Levels: " << nLevels << endl;
        cout << "- Scale Factor: " << fScaleFactor << endl;
        cout << "- Initial Fast Threshold: " << fIniThFAST << endl;
        cout << "- Minimum Fast Threshold: " << fMinThFAST << endl;

        if (sensor == System::STEREO || sensor == System::RGBD)
        {
            // 判断3D点远近的阈值。mbf*ThDep/fx=基线*倍率
            mThDepth = mbf * (float) fSettings["ThDepth"] / fx;
            cout << endl << "Depth Threshold (Close/Far Points): " << mThDepth << endl;
        }

        if (sensor == System::RGBD)
        {
            // 深度相机disparity转化为depth时的因子。
            mDepthMapFactor = fSettings["DepthMapFactor"];
            if (mDepthMapFactor == 0)
                mDepthMapFactor = 1;
            else
                mDepthMapFactor = 1.0f / mDepthMapFactor;
        }

    }

    // 设置进程间对象指针。
    void Tracking::SetLocalMapper(LocalMapping *pLocalMapper)
    {
        mpLocalMapper = pLocalMapper;
    }

    void Tracking::SetLoopClosing(LoopClosing *pLoopClosing)
    {
        mpLoopClosing = pLoopClosing;
    }

    void Tracking::SetViewer(Viewer *pViewer)
    {
        mpViewer = pViewer;
    }

    // 输入左右目图像，可以为RGB、BGR、RGBA、GRAY
    // 1、将图像转为mImGray和imGrayRight并初始化mCurrentFrame
    // 2、进行tracking过程
    // 输出世界坐标系到该帧相机坐标系的变换矩阵
    cv::Mat Tracking::GrabImageStereo(const cv::Mat &imRectLeft, const cv::Mat &imRectRight, const double &timestamp)
    {
        mImGray = imRectLeft;
        cv::Mat imGrayRight = imRectRight;

        // 步骤1：将RGB或RGBA图像转为灰度图像
        if (mImGray.channels() == 3)
        {
            if (mbRGB)
            {
                cvtColor(mImGray, mImGray, CV_RGB2GRAY);
                cvtColor(imGrayRight, imGrayRight, CV_RGB2GRAY);
            }
            else
            {
                cvtColor(mImGray, mImGray, CV_BGR2GRAY);
                cvtColor(imGrayRight, imGrayRight, CV_BGR2GRAY);
            }
        }
        else if (mImGray.channels() == 4)
        {
            if (mbRGB)
            {
                cvtColor(mImGray, mImGray, CV_RGBA2GRAY);
                cvtColor(imGrayRight, imGrayRight, CV_RGBA2GRAY);
            }
            else
            {
                cvtColor(mImGray, mImGray, CV_BGRA2GRAY);
                cvtColor(imGrayRight, imGrayRight, CV_BGRA2GRAY);
            }
        }

        // 步骤2：构造Frame
        // mCurrentFrame = Frame(mImGray,imGrayRight,timestamp,mpORBextractorLeft,mpORBextractorRight,mpORBVocabulary,mK,mDistCoef,mbf,mThDepth);
        mCurrentFrame = Frame(mImGray, imGrayRight, timestamp, mpORBextractorLeft, mpORBextractorRight, mpORBVocabulary,
                              mK, mDistCoef, mbf, mThDepth);
        // 步骤3：跟踪
        Track();

        return mCurrentFrame.mTcw.clone();
    }

    // 输入左目RGB或RGBA图像和深度图
    // 1、将图像转为mImGray和imDepth并初始化mCurrentFrame
    // 2、进行tracking过程
    // 输出世界坐标系到该帧相机坐标系的变换矩阵
    cv::Mat Tracking::GrabImageRGBD(const cv::Mat &imRGB, const cv::Mat &imD, const double &timestamp)
    {
        mImGray = imRGB;
        cv::Mat imDepth = imD;

        // 步骤1：将RGB或RGBA图像转为灰度图像
        if (mImGray.channels() == 3)
        {
            if (mbRGB)
                cvtColor(mImGray, mImGray, CV_RGB2GRAY);
            else
                cvtColor(mImGray, mImGray, CV_BGR2GRAY);
        }
        else if (mImGray.channels() == 4)
        {
            if (mbRGB)
                cvtColor(mImGray, mImGray, CV_RGBA2GRAY);
            else
                cvtColor(mImGray, mImGray, CV_BGRA2GRAY);
        }

        // 步骤2：将深度相机的disparity转为Depth
        if (mDepthMapFactor != 1 || imDepth.type() != CV_32F);
        imDepth.convertTo(imDepth, CV_32F, mDepthMapFactor);

        // 步骤3：构造Frame
        mCurrentFrame = Frame(mImGray, imDepth, timestamp, mpORBextractorLeft, mpORBVocabulary, mK, mDistCoef, mbf,
                              mThDepth);

        // 步骤4：跟踪
        Track();

        return mCurrentFrame.mTcw.clone();
    }


    // 输入左目RGB或RGBA图像和时间戳。
    // 1.把图像转换为灰度图像，初始化为mCurrentFrame。
    // 2.进行Tracking过程(Track()成员函数)。
    // 输出世界坐标系到帧镇相机坐标系的变换矩阵。
    cv::Mat Tracking::GrabImageMonocular(const cv::Mat &im, const double &timestamp)
    {

        mImGray = im;

        // 步骤1：将RGB或RGBA图像转换为灰度图像。
        if (mImGray.channels() == 3)
        {
            if (mbRGB)
                cvtColor(mImGray, mImGray, CV_RGB2GRAY);
            else
                cvtColor(mImGray, mImGray, CV_BGR2GRAY);
        }
        else if (mImGray.channels() == 4)
        {
            if (mbRGB)
                cvtColor(mImGray, mImGray, CV_RGBA2GRAY);
            else
                cvtColor(mImGray, mImGray, CV_BGRA2GRAY);
        }
        // 步骤2：构造Frame。
        if (mState == NOT_INITIALIZED || mState == NO_IMAGES_YET)       // 没有进行初始化或者初始化没有成功。
            mCurrentFrame = Frame(mImGray, timestamp, mpIniORBextractor, mpORBVocabulary, mK, mDistCoef, mbf, mThDepth);
        else
            mCurrentFrame = Frame(mImGray, timestamp, mpORBextractorLeft, mpORBVocabulary, mK, mDistCoef, mbf,
                                  mThDepth);

        // 步骤3：Tracking线程入口
        Track();

        return mCurrentFrame.mTcw.clone();
    }


    // Tracking线程的主函数，Track(), 独立于输入传感器类型，单目，双目，RGBD都用这个进行跟踪。
    // VI SLAM和VSLAM相比，加入了IMU运动跟踪和IMU局部地图跟踪，并且在lost重定位成功后，通过20帧Frame估计bias
    // track()完成两个部分  运动估计和局部地图跟踪。
    // track()：a.初始化(Initial)。b.状态正常跟踪，丢失则重定位，PnP得到初始位姿；局部地图跟踪；检测是否插入新的关键帧。c.记录位姿信息。
    // 跟踪：1.检查上一帧地图点云的变化；2.特征跟踪（存在运动模型优先恒速跟踪，失效则进行BoW参考帧跟踪）；3.局部地图跟踪；4.更新速率模型，关键帧插入；5.判断跟踪效果，保存当前帧数据到上一帧。
    // 输入：	mpTracker对象。
    // 输出：	mpLocalMapper对象(主要是更新关键帧，通过mpLocalMapper->InsertKeyFrame添加到mlNewKeyFrames中)。
    void Tracking::Track()
    {

        // mState表示Tracking的状态。
        // NO_IMAGES_YET表示第一次运行或者复位过。
        if (mState == NO_IMAGES_YET)
        {
            mState = NOT_INITIALIZED;
        }

        // mLastProcessedState记录Tracking上次的状态。
        mLastProcessedState = mState;

        // 进入Map mutex(对象互斥锁)，保持当前地图不变。
        unique_lock<mutex> lock(mpMap->mMutexMapUpdate);

        // 判断地图是否更新
        bool bMapUpdated = false;
        if (mpLocalMapper->GetMapUpdateFlagForTracking())
        {
            bMapUpdated = true;
            mpLocalMapper->SetMapUpdateFlagInTracking(false);
        }
        if (mpLoopClosing->GetMapUpdateFlagForTracking())
        {
            bMapUpdated = true;
            mpLoopClosing->SetMapUpdateFlagInTracking(false);
        }
        if (mCurrentFrame.mnId == mnLastRelocFrameId + 20)
        {
            bMapUpdated = true;
        }

        // 步骤1 初始化，判断使用单应还是基本矩阵，计算两帧间相对位姿，三角原理初始化一组点云。2D-2D,3D-2D
        if (mState == NOT_INITIALIZED)
        {
            if (mSensor == System::STEREO || mSensor == System::RGBD)
                StereoInitialization();
            else
                MonocularInitialization();

            mpFrameDrawer->Update(this);

            if (mState != OK)
                return;
        }



            // 步骤2 局部跟踪。包括运动模型跟踪，参考帧跟踪，重定位和局部地图跟踪。
        else
        {
            // bOK表示每个函数是否执行成功。
            bool bOK = false;

            // 使用恒速运动模型或参考帧跟踪估计相机位姿，如果丢失使用重定位估计。
            // 跟踪CF,寻找与KF中的匹配特征点；恒速模型(或参考帧)，PnP求解R,t；
            // mbOnlyTracking=false表示正常模式(有地图更新)，否则表示用户选择了定位模式。
            // SLAM模式
            if (!mbOnlyTracking)
            {
                //启动局部地图。

                // 初始化或跟Track态正常。
                if (mState == OK)
                {
                    // 检查并更新上一帧被替换的MapPoints
                    // 更新Fuse函数和SearchAndFuse函数替换的MapPoints
                    CheckReplacedInLastFrame();
#ifdef TRACK_WITH_IMU
                    // 判断是否完成VI初始化
                    if (mpLocalMapper->GetVINSInited())
                    {

                        //刚刚完成重定位
                        if (mbRelocBiasPrepare)
                        {
                            bOK = TrackReferenceKeyFrame();
                        }
                        else
                        {
                            // IMU Track
                            bOK = TrackWithIMU(bMapUpdated);
                            if (!bOK)
                                bOK = TrackReferenceKeyFrame();
                        }

                    }
                        // 没有完成VI初始化, 纯视觉SLAM
                    else
#endif
                    {

                        // 步骤2.1 跟踪上一帧或参考关键帧或者重定位帧。
                        // 运动模式为空或刚刚完成重定位。
                        // mnLastRelocFrameId表示上次重定位的那一帧。
                        if (mVelocity.empty() || mCurrentFrame.mnId < mnLastRelocFrameId + 2)
                        {
                            // 上一帧的位姿作为当前帧的初始化位姿。
                            // 通过BoW的方式在当前帧中找到参考帧特征点的匹配点。
                            // 最小化3D点的重投影误差得到位姿。
                            bOK = TrackReferenceKeyFrame();
                        }

                        else
                        {
                            // 根据恒速模型设定当前帧的初始位姿。
                            // 通过投影的方式在当前帧中找到参考帧的匹配点。
                            // 最小化3D重投影误差得到位姿。
                            bOK = TrackWithMotionModel();

                            // 没有跟踪成功时，跟踪参考帧，BoW加速匹配跟踪。
                            if (!bOK)
                                bOK = TrackReferenceKeyFrame();

                        }
                    }
                }

                    // 当前状态不是ok， 重定位。
                else
                {
                    // BoW搜索，PnP求解位姿。
                    bOK = Relocalization();
                    if (bOK)
                        cout << "Relocalized. id: " << mCurrentFrame.mnId << endl;
                }
            }

            else
            {
                cerr << "Localization mode not supported yet" << endl;
            }


//             // 定位模式，不进行局部地图。
//             else
//             {
//                 // 步骤2.1 跟踪上一帧||参考帧||重定位。
// 
//                 // 跟踪丢失，重定位。
//                 if(mState==LOST)
//                 {
//                     bOK = Relocalization();
//                 }
// 
//                 else
//                 {
//                     // mbVO是跟踪模式下出现的变量。
//                     // mbVO=false 表示跟踪正常，很多匹配；  mbVO=true 表示匹配了很少的地图点云，要GG。
//                     
//                     // 跟踪正常。
//                     if(!mbVO)
//                     {
//                         if(!mVelocity.empty())
//                         {
//                             // 恒速模型跟踪。
//                             bOK = TrackWithMotionModel();
//                             
//                             // 恒速模型失败，参考帧跟踪。
// 							if(!bOK)
//                               bOK = TrackReferenceKeyFrame();
//                         }
// 
//                         // 没有恒速跟踪模型。
//                         else
//                         {
//                             bOK = TrackReferenceKeyFrame();
//                         }
//                     }
// 
//                     // 跟踪效果不好，要GG了。
//                     else
//                     {
// 
//                         // 同时进行跟踪和重定位。
//                         // 如果重定位成功，使用重定位的结果。否则保持跟踪视觉里里程计点。
//                         bool bOKMM = false;         // 恒速模型运行标志位。
//                         bool bOKReloc =false;       // 重定位运行标志位。
//                         vector<MapPoint *> vpMPsMM; // 恒速模型匹配地图点云
//                         vector<bool> vbOutMM;       // 恒速模型匹配异常值情况。
//                         cv::Mat TcwMM;              // 恒速模型位姿。
// 
//                         // 进行恒速模型匹配。
//                         if(!mVelocity.empty())
//                         {
//                             bOKMM = TrackWithMotionModel();
//                             vpMPsMM = mCurrentFrame.mvpMapPoints;
//                             vbOutMM = mCurrentFrame.mvbOutlier;
//                             TcwMM = mCurrentFrame.mTcw.clone();
//                         }
//                         // 进行重定位。
//                         bOKReloc = Relocalization();
// 
//                         // 跟踪成功，重定位失败。
//                         if(bOKMM && !bOKReloc)
//                         {
//                             mCurrentFrame.SetPose(TcwMM);
//                             mCurrentFrame.mvpMapPoints = vpMPsMM;
//                             mCurrentFrame.mvbOutlier = vbOutMM;
//                             
//                             // 处理的不是一个东西，应该在TrackLocalMap函数中吧。
//                             // 更新当前帧的MapPoints被观测程度。
//                             if (mbVO)
//                             {
//                                for (int i=0; i<mCurrentFrame.N; i++)
//                                {
//                                     // 有匹配点，且不是外点，加入。
//                                     if(mCurrentFrame.mvpMapPoints[i] && !mCurrentFrame.mvbOutlier[i])
//                                     {
//                                         mCurrentFrame.mvpMapPoints[i]->IncreaseFound();
//                                     }
//                                }
//                             }
//                         }
//                         // 重定位成功表示跟踪正常，更相信定位结果。
//                         else if(bOKReloc)
//                         {
//                             mbVO = false;
//                         }
// 
//                         bOK = bOKReloc || bOKMM;
//                     }   // 跟踪效果不好
//                 }   // 没跟丢
//             }   // 定位模式 

            // 将最新的关键帧作为参考帧, mpReferenceKF在初始化MonocularInitialization()中完成。
            mCurrentFrame.mpReferenceKF = mpReferenceKF;


            // 步骤2.2 完成初始化，通过帧间匹配得到初始位姿和匹配点云后，对现有的局部地图进行跟踪得到更多匹配，优化当前位姿。
            // 局部地图：当前帧CF,当前帧的点云地图MapPoint，当前关键帧与其他关键帧的共视关系。
            // 步骤2.1主要是帧间两两跟踪(跟踪上一帧、参考帧)，这里的局部地图搜索是关键帧(参考帧)后的全部地图点MapPoints。
            // 将局部MapPoint和当前的帧进行投影匹配，得到更多匹配的MapPoint后进行Pose优化(Covisible graph)。

            // SLAM模式，使能局部地图。
            if (!mbOnlyTracking)
            {
                if (bOK)
                {
#ifndef TRACK_WITH_IMU
                    bOK = TrackLocalMap();
#else
                    // IMU TrackLocal Map
                    if (!mpLocalMapper->GetVINSInited())
                        bOK = TrackLocalMap();
                    else
                    {
                        // 刚完成重定位
                        if (mbRelocBiasPrepare)
                        {
                            bOK = TrackLocalMap();
                        }
                        else
                        {
                            bOK = TrackLocalMapWithIMU(bMapUpdated);
                        }
                    }
#endif
                }
            }
                // 定位模式。
            else
            {
                cerr << "Localization mode not supported yet" << endl;
                // mbVO=true 表示地图效果不好，不能提取局部地图因而无法运行TrackLocalMap(),如果系统重定位，执行局部地图构建。
//                 if(bOK && !mbVO)
//                     bOK = TrackLocalMap();
            }

            // bOK表示之前所有过程的结果，如果完成表示Tracking成功，否则只要有一个有问题，就GG。
            if (bOK)
            {
                mState = OK;

                // 在重定位后，重新计算IMU bias
                if (mbRelocBiasPrepare)
                {
                    mv20FramesReloc.push_back(mCurrentFrame);

                    // 创建关键帧前，重新计算IMU偏移
                    if (mCurrentFrame.mnId == mnLastRelocFrameId + 19)
                    {
                        NavState nscur;
                        RecomputeIMUBiasAndCurrentNavstate(nscur);

                        // 更新当前帧Nav
                        mCurrentFrame.SetNavState(nscur);

                        // 清空标志和Frame缓存
                        mbRelocBiasPrepare = false;
                        mv20FramesReloc.clear();

                        // 使能Local mapping
                        mpLocalMapper->Release();

                        // 创建新关键帧
                        mbCreateNewKFAfterReloc = true;

                        // test log
                        cout << "NavState recomputed." << endl;
                        cout << "V:" << mCurrentFrame.GetNavState().Get_V().transpose() << endl;
                        cout << "bg:" << mCurrentFrame.GetNavState().Get_BiasGyr().transpose() << endl;
                        cout << "ba:" << mCurrentFrame.GetNavState().Get_BiasAcc().transpose() << endl;
                        cout << "dbg:" << mCurrentFrame.GetNavState().Get_dBias_Gyr().transpose() << endl;
                        cout << "dba:" << mCurrentFrame.GetNavState().Get_dBias_Acc().transpose() << endl;
                    }
                }
            }
            else
            {
                mState = LOST;

                if (mv20FramesReloc.size() > 0)
                    mv20FramesReloc.clear();
            }
            // 更新UI绘制。
            mpFrameDrawer->Update(this);


            // 如果跟踪效果很好，更新速率模型，判断是否插入关键帧。
            if (bOK)
            {
                // 更新运动模型。
                if (!mLastFrame.mTcw.empty())
                {

                    // 步骤2.3 更新恒速模型中的速率
                    cv::Mat LastTwc = cv::Mat::eye(4, 4, CV_32F);
                    mLastFrame.GetRotationInverse().copyTo(LastTwc.rowRange(0, 3).colRange(0, 3));
                    mLastFrame.GetCameraCenter().copyTo(LastTwc.rowRange(0, 3).col(3));
                    mVelocity = mCurrentFrame.mTcw * LastTwc; // Tcl 上一帧到当前帧的位姿变换。
                }
                    // 运动模型为空
                else
                    mVelocity = cv::Mat();

                mpMapDrawer->SetCurrentCameraPose(mCurrentFrame.mTcw);

                // 仅双目和RGBD。
                // 步骤2.4 清除UpdateLastFrame中为当前帧临时添加的MapPoints
                for (int i = 0; i < mCurrentFrame.N; i++)
                {
                    MapPoint *pMP = mCurrentFrame.mvpMapPoints[i];
                    if (pMP)
                        // 剔除UpdateLastFrame函数中为了跟踪增加的MapPoints
                        if (pMP->Observations() < 1)
                        {
                            mCurrentFrame.mvbOutlier[i] = false;
                            mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint *>(NULL);
                        }
                }
                // 步骤2.5 清除临时的MapPoints，这些MapPoints在TrackWithMotionModel的UpdateLastFrame函数里生成（仅双目和rgbd）
                // 步骤2.4中只是在当前帧中将这些MapPoints剔除，这里释放MapPoints
                // 这里生成的仅仅是为了提高双目或rgbd摄像头的帧间跟踪效果，用完以后就扔了，没有添加到地图中
                for (list<MapPoint *>::iterator lit = mlpTemporalPoints.begin(), lend = mlpTemporalPoints.end();
                     lit != lend; lit++)
                {
                    MapPoint *pMP = *lit;
                    delete pMP;
                }
                // 这里不仅仅是清除mlpTemporalPoints，通过delete pMP还删除了指针指向的MapPoint
                mlpTemporalPoints.clear();

                // 步骤2.6 检测并插入关键帧，对于双目会产生新的MapPoints。
                if (NeedNewKeyFrame() || mbCreateNewKFAfterReloc)
                    CreateNewKeyFrame();

                // 清除标志
                if (mbCreateNewKFAfterReloc)
                    mbCreateNewKFAfterReloc = false;

                // 删除在PnP bundle adjustment中检测为离群值的3D点。 
                for (int i = 0; i < mCurrentFrame.N; i++)
                {
                    if (mCurrentFrame.mvpMapPoints[i] && mCurrentFrame.mvbOutlier[i])
                        mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint *>(NULL);
                }

                // 清除第一次初始标志
                if (mpLocalMapper->GetFirstVINSInited())
                {
                    mpLocalMapper->SetFirstVINSInited(false);
                }

            }   // 更新速率模型，插入关键帧判断。

            // 跟踪失败，且没有完成VI初始化(地图点很少)，使能Reset 
            if (mState == LOST)
            {
                // if(mpMap->KeyFramesInMap()<=5)
                if (!mpLocalMapper->GetVINSInited())
                {
                    // cout << "Track lost soon after initialisation, reseting ... " <<endl;
                    cout << "Track lost before VI initialisation, reseting ... " << endl;
                    mpSystem->Reset();
                    return;
                }
            }   // 跟踪、定位失败，Reset

            if (!mCurrentFrame.mpReferenceKF)
                mCurrentFrame.mpReferenceKF = mpReferenceKF;

            // 保存上一帧的数据。
            mLastFrame = Frame(mCurrentFrame);

        }   // 步骤2 局部跟踪。



        // 步骤3 记录位姿信息，用于轨迹复现。
        if (!mCurrentFrame.mTcw.empty())
        {
            // 计算相对姿态 T_currentFrame_referenceKeyFrame
            cv::Mat Tcr = mCurrentFrame.mTcw * mCurrentFrame.mpReferenceKF->GetPoseInverse();
            mlRelativeFramePoses.push_back(Tcr);
            mlpReferences.push_back(mpReferenceKF);
            mlFrameTimes.push_back(mCurrentFrame.mTimeStamp);
            mlbLost.push_back(mState == LOST);
        }
        else
        {
            // 如果跟踪失败，相对位姿使用上一次的值。
            mlRelativeFramePoses.push_back(mlRelativeFramePoses.back());
            mlpReferences.push_back(mlpReferences.back());
            mlFrameTimes.push_back(mlFrameTimes.back());
            mlbLost.push_back(mState == LOST);
        }

    }   // Tracking();





    // brief 双目和rgbd的地图初始化
    // 由于具有深度信息，直接生成MapPoints
    void Tracking::StereoInitialization()
    {
        if (mCurrentFrame.N > 500)
        {
            // Set Frame pose to the origin
            // 步骤1：设定初始位姿
            mCurrentFrame.SetPose(cv::Mat::eye(4, 4, CV_32F));

            // Create KeyFrame
            // 步骤2：将当前帧构造为初始关键帧
            // mCurrentFrame的数据类型为Frame
            // KeyFrame包含Frame、地图3D点、以及BoW
            // KeyFrame里有一个mpMap，Tracking里有一个mpMap，而KeyFrame里的mpMap都指向Tracking里的这个mpMap
            // KeyFrame里有一个mpKeyFrameDB，Tracking里有一个mpKeyFrameDB，而KeyFrame里的mpMap都指向Tracking里的这个mpKeyFrameDB
            KeyFrame *pKFini = new KeyFrame(mCurrentFrame, mpMap, mpKeyFrameDB);

            // Insert KeyFrame in the map
            // KeyFrame中包含了地图、反过来地图中也包含了KeyFrame，相互包含
            // 步骤3：在地图中添加该初始关键帧
            mpMap->AddKeyFrame(pKFini);

            // Create MapPoints and asscoiate to KeyFrame
            // 步骤4：为每个特征点构造MapPoint
            for (int i = 0; i < mCurrentFrame.N; i++)
            {
                float z = mCurrentFrame.mvDepth[i];
                if (z > 0)
                {
                    // 步骤4.1：通过反投影得到该特征点的3D坐标
                    cv::Mat x3D = mCurrentFrame.UnprojectStereo(i);
                    // 步骤4.2：将3D点构造为MapPoin
                    MapPoint *pNewMP = new MapPoint(x3D, pKFini, mpMap);

                    // 步骤4.3：为该MapPoint添加属性：
                    // a.观测到该MapPoint的关键帧
                    // b.该MapPoint的描述子
                    // c.该MapPoint的平均观测方向和深度范围

                    // a.表示该MapPoint可以被哪个KeyFrame的哪个特征点观测到
                    pNewMP->AddObservation(pKFini, i);
                    // b.从众多观测到该MapPoint的特征点中挑选区分读最高的描述子
                    pNewMP->ComputeDistinctiveDescriptors();
                    // c.更新该MapPoint平均观测方向以及观测距离的范围
                    pNewMP->UpdateNormalAndDepth();

                    // 步骤4.4：在地图中添加该MapPoint
                    mpMap->AddMapPoint(pNewMP);
                    // 步骤4.5：表示该KeyFrame的哪个特征点可以观测到哪个3D点
                    pKFini->AddMapPoint(pNewMP, i);

                    // 步骤4.6：将该MapPoint添加到当前帧的mvpMapPoints中
                    // 为当前Frame的特征点与MapPoint之间建立索引
                    mCurrentFrame.mvpMapPoints[i] = pNewMP;
                }
            }

            cout << "New map created with " << mpMap->MapPointsInMap() << " points" << endl;

            // 步骤4：在局部地图中添加该初始关键帧
            mpLocalMapper->InsertKeyFrame(pKFini);

            mLastFrame = Frame(mCurrentFrame);
            mnLastKeyFrameId = mCurrentFrame.mnId;
            mpLastKeyFrame = pKFini;

            mvpLocalKeyFrames.push_back(pKFini);
            mvpLocalMapPoints = mpMap->GetAllMapPoints();
            mpReferenceKF = pKFini;
            mCurrentFrame.mpReferenceKF = pKFini;

            // 把当前（最新的）局部MapPoints作为ReferenceMapPoints
            // ReferenceMapPoints是DrawMapPoints函数画图的时候用的
            mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

            mpMap->mvpKeyFrameOrigins.push_back(pKFini);

            mpMapDrawer->SetCurrentCameraPose(mCurrentFrame.mTcw);

            mState = OK;
        }
    }


    // 单目地图初始化
    // 并行计算基础矩阵F和单应矩阵H,选取其中的一个，恢复初始两帧之前的相对位姿和点云。
    // 初始两帧的匹配，相对运动、初始MapPoints。
    void Tracking::MonocularInitialization()
    {
        // 没有单目初始器，创建单目初始器。
        if (!mpInitializer)
        {
            // 清空从上一帧关键帧到当前帧的IMU数据
            mvIMUSinceLastKF.clear();
            // 初始帧的特征数>100
            if (mCurrentFrame.mvKeys.size() > 100)
            {
                // 步骤1 得到用于初始化的第一帧，初始化需要两帧。
                mInitialFrame = Frame(mCurrentFrame);
                // 保存当前帧状态，用于获取第二帧初始帧后进行匹配。
                mLastFrame = Frame(mCurrentFrame);
                // mvbPrevMatched最大的情况就是所有的特征都被跟踪了。
                mvbPrevMatched.resize(mCurrentFrame.mvKeysUn.size());
                for (size_t i = 0; i < mCurrentFrame.mvKeysUn.size(); i++)
                    mvbPrevMatched[i] = mCurrentFrame.mvKeysUn[i].pt;

                // 防止引用和野指针创建。
                if (mpInitializer)
                    delete mpInitializer;

                // 由当前帧构造初始器，sigma:1.0 迭代次数200
                mpInitializer = new Initializer(mCurrentFrame, 1.0, 200);

                // mvIniMatches保存两帧之间的匹配特征点Id，初始化时没有匹配，填充-1。
                fill(mvIniMatches.begin(), mvIniMatches.end(), -1);

                return;
            }
        }   // 没有单目初始器，创建单目初始器。

            // 已有单目初始器。
        else
        {
            // 步骤2 特征点数>100，得到初始化的第二帧。
            // 只要连续两帧的特征点都大于100，才能继续初始化，否则重新构造初始器。
            if ((int) mCurrentFrame.mvKeys.size() <= 100)
            {
                delete mpInitializer;
                mpInitializer = static_cast<Initializer *>(NULL);
                fill(mvIniMatches.begin(), mvIniMatches.end(), -1);
                return;
            }

            // 在mInitialFrame与mCurrentFrame中找匹配对应特征点对。
            // 步骤3 mvbPrevMatched存储前一帧特征点。
            // mvIniMatches存储mInitialFrame与mCurrentFrame之间匹配的特征点id。
            ORBmatcher matcher(0.9, true);
            int nmatches = matcher.SearchForInitialization(mInitialFrame, mCurrentFrame, mvbPrevMatched, mvIniMatches,
                                                           100);

            // 步骤4 如果初始化两帧之间匹配特征点太少，重新初始化。
            if (nmatches < 100)
            {
                delete mpInitializer;
                mpInitializer = static_cast<Initializer *>(NULL);
                return;
            }

            cv::Mat Rcw;    // 当前相机的旋转矩阵。
            cv::Mat tcw;    // 当前相机的位移矩阵。
            vector<bool> vbTriangulated;    // mvIniMatches中特征点对的三角化标志。

            // 步骤5 通过H模型或F模型进行单目初始化(mpInitializer->Initialize)，得到两帧间的相对运动，初始MapPoints
            if (mpInitializer->Initialize(mCurrentFrame, mvIniMatches, Rcw, tcw, mvIniP3D, vbTriangulated))
            {

                // 步骤6 剔除无法进行三角化的匹配点。
                for (size_t i = 0, iend = mvIniMatches.size(); i < iend; i++)
                {
                    if (mvIniMatches[i] >= 0 && !vbTriangulated[i])
                    {
                        mvIniMatches[i] = -1;
                        nmatches--;
                    }
                }

                // 存储初始化得到的位姿。
                // 设置帧位姿，初始化的第一帧为世界坐标系，变换矩阵是单位矩阵。
                mInitialFrame.SetPose(cv::Mat::eye(4, 4, CV_32F));
                // 根据Rcw和tcw构造Tcw，赋值给mTcw(世界坐标系到该帧的变换矩阵，即相对初始帧的相对位姿)
                cv::Mat Tcw = cv::Mat::eye(4, 4, CV_32F);
                Rcw.copyTo(Tcw.rowRange(0, 3).colRange(0, 3));
                tcw.copyTo(Tcw.rowRange(0, 3).col(3));
                mCurrentFrame.SetPose(Tcw);

                // 步骤7 将三角化得到的点云包装成MapPoints。
                // Initialize()得到mvIniP3D, 是一个cv::Point3f类型的容器，存放3D点的临时变量。
                // 下面的函数将3D点包装成MapPoint类型存入KeyFrame和Map中。
                CreateInitialMapMonocular();

            }   // H和F模型进行单目初始化。

        }   // 已有初始器。

    }   // MonocularInitialization



    // 单目摄像头三角化生成MapPoints
    void Tracking::CreateInitialMapMonocular()
    {
        // KF1 和 KF2 之间的IMU
        vector<IMUData> vimu1, vimu2;
        for (size_t i = 0; i < mvIMUSinceLastKF.size(); i++)
        {
            IMUData imu = mvIMUSinceLastKF[i];
            if (imu._t < mInitialFrame.mTimeStamp)
                vimu1.push_back(imu);
            else
                vimu2.push_back(imu);
        }

        // 利用初始化的前两帧创建关键帧。
        //  KeyFrame *pKFini = new KeyFrame(mInitialFrame, mpMap, mpKeyFrameDB);
        KeyFrame *pKFini = new KeyFrame(mInitialFrame, mpMap, mpKeyFrameDB, vimu1, NULL);
        pKFini->ComputePreInt();

        // KeyFrame *pKFcur = new KeyFrame(mCurrentFrame, mpMap, mpKeyFrameDB);
        KeyFrame *pKFcur = new KeyFrame(mCurrentFrame, mpMap, mpKeyFrameDB, vimu2, pKFini);
        pKFcur->ComputePreInt();

        // 清空上一帧关键帧到当前帧的IMU数据
        mvIMUSinceLastKF.clear();

        // 步骤1 初始化关键帧的描述子转为BoW。
        pKFini->ComputeBoW();

        // 步骤2 将当前关键帧的描述自转为BoW。
        pKFcur->ComputeBoW();

        // 步骤3 将两帧关键帧插入到地图中。
        mpMap->AddKeyFrame(pKFini);
        mpMap->AddKeyFrame(pKFcur);

        // 步骤4 创建MapPoints和关联的关键帧。
        for (size_t i = 0; i < mvIniMatches.size(); i++)
        {
            if (mvIniMatches[i] < 0)
                continue;

            // 创建地图点云。
            cv::Mat worldPos(mvIniP3D[i]);

            // 步骤4.1 用3D点构造MapPoint。
            MapPoint *pMP = new MapPoint(worldPos, pKFcur, mpMap);

            // 步骤4.2 KeyFrame中的哪个观测点对应3D点。
            pKFini->AddMapPoint(pMP, i);
            pKFcur->AddMapPoint(pMP, mvIniMatches[i]);

            // 步骤4.3 添加关键帧点云属性。
            // a  观测到的该MapPoint的关键帧
            // b  该MapPoint的描述子。
            // c  该MapPoint的平均观测方向和深度范围


            // a 该MapPoint可以被哪个KeyFrame的哪个特征点观测到。
            pMP->AddObservation(pKFini, i);
            pMP->AddObservation(pKFcur, mvIniMatches[i]);
            // b 提取该MapPoint对应的所有特征点中区分度最高的描述子。
            pMP->ComputeDistinctiveDescriptors();
            // c 更新该MapPoint的平均观测方向以及观测距离的范围。
            pMP->UpdateNormalAndDepth();

            // 步骤4.3 KeyFrame中的哪个观测点对应3D点。
            // pKFini->AddMapPoint(pMP, i);
            // pKFcur->AddMapPoint(pMP, mvIniMatches[i]);

            // 更新当前帧结构。
            mCurrentFrame.mvpMapPoints[mvIniMatches[i]] = pMP;
            mCurrentFrame.mvbOutlier[mvIniMatches[i]] = false;

            // 步骤4.4 在地图中添加MapPoint
            mpMap->AddMapPoint(pMP);

        }

        // 步骤5 更新关键帧间连接关系，Covisible graph。
        // 节点时关键帧，边表示两个关键帧观察到的相同MapPoints数量
        pKFini->UpdateConnections();
        pKFcur->UpdateConnections();

        // 步骤6 全局BA优化初始化结果。
        cout << "New Map created with " << mpMap->MapPointsInMap() << "points" << endl;
        Optimizer::GlobalBundleAdjustment(mpMap, 20);

        // 步骤7 将MapPoint的中值深度归一化到1，并归一化两帧之间的变换。
        // 评估关键帧场景深度，q=2表示值。
        float medianDepth = pKFini->ComputeSceneMedianDepth(2);
        float invMedianDepth = 1.0f / medianDepth;

        if (medianDepth < 0 || pKFcur->TrackedMapPoints(1) < 100)
        {
            cout << "Wrong initialization, reseting..." << endl;
            Reset();
            return;
        }

        // 缩放初始基线。
        cv::Mat Tc2w = pKFcur->GetPose();
        // 位移矩阵t  x/z, y/z 将z归一化到1
        Tc2w.col(3).rowRange(0, 3) = Tc2w.col(3).rowRange(0, 3) * invMedianDepth;
        pKFcur->SetPose(Tc2w);

        // 3D点尺度归一化到1。
        vector<MapPoint *> vpAllMapPoints = pKFini->GetMapPointMatches();
        for (size_t iMP = 0; iMP < vpAllMapPoints.size(); iMP++)
        {
            if (vpAllMapPoints[iMP])
            {
                MapPoint *pMP = vpAllMapPoints[iMP];
                pMP->SetWorldPos(pMP->GetWorldPos() * invMedianDepth);
            }
        }

        // 将初始化关键帧加入局部地图。
        mpLocalMapper->InsertKeyFrame(pKFini);
        mpLocalMapper->InsertKeyFrame(pKFcur);

        mCurrentFrame.SetPose(pKFcur->GetPose());
        mnLastKeyFrameId = mCurrentFrame.mnId;
        mpLastKeyFrame = pKFcur;

        mvpLocalKeyFrames.push_back(pKFcur);
        mvpLocalKeyFrames.push_back(pKFini);
        mvpLocalMapPoints = mpMap->GetAllMapPoints();
        mpReferenceKF = pKFcur;                            // 初始化第二帧是参考关键帧
        mCurrentFrame.mpReferenceKF = pKFcur;

        mLastFrame = Frame(mCurrentFrame);

        // 把当前最新的局部MapPoints作为ReferenceMapPoints，用于画图时DrawMapPoint使用。
        mpMap->SetReferenceMapPoints(mvpLocalMapPoints);
        mpMapDrawer->SetCurrentCameraPose(pKFcur->GetPose());
        mpMap->mvpKeyFrameOrigins.push_back(pKFini);
        mState = OK; // 初始化成功，初始化过程完成。
    }   // CreateInitialMapMonocular



    // 替换上一帧中某些MapPoints, LocalMapping线程可能会修改上一帧的某些MapPoint。
    void Tracking::CheckReplacedInLastFrame()
    {
        for (int i = 0; i < mLastFrame.N; i++)
        {
            MapPoint *pMP = mLastFrame.mvpMapPoints[i];

            if (pMP)
            {
                MapPoint *pRep = pMP->GetReplaced();
                if (pRep)
                    mLastFrame.mvpMapPoints[i] = pRep;

            }
        }
    }


    // 对参考关键帧的MapPoint进行跟踪，一般情况下的参考帧是当前关键帧或者与当前帧公视最高的关键帧。
    // 计算位姿没有使用EPnP, 对当前帧赋予参考帧位姿，BA迭代优化。
    // 1.计算当前帧的BoW，将当前帧的特征点分到特定层的nodes上。
    // 2.对属于同一node的描述子进行匹配。
    // 3.根据匹配特征估计当前帧的姿态。
    // 4.根据姿态剔除误匹配。
    // 如果匹配数目大于10，返回true
    bool Tracking::TrackReferenceKeyFrame()
    {
        // 步骤1 将当前特征描述自转换维BoW
        mCurrentFrame.ComputeBoW();


        // 步骤2 通过特征点的BoW加快当前帧与参考帧之间的特征点匹配。
        ORBmatcher matcher(0.7, true);
        vector<MapPoint *> vpMapPointMatches;        // 匹配点云。
        // 通过词典，对关键帧中的地图点云和当前帧中的ORB特征进行匹配，为了加速匹配过程，关键帧和当前帧的描述子划分道特定层的nodes中。
        int nmatches = matcher.SearchByBoW(mpReferenceKF, mCurrentFrame, vpMapPointMatches);

        if (nmatches < 15)
            return false;

        // 步骤3 将上一帧的位姿作为当前帧位姿的初始值。
        mCurrentFrame.mvpMapPoints = vpMapPointMatches;
        mCurrentFrame.SetPose(mLastFrame.mTcw);

        // 步骤4 通过BA-PnP求解器优化3D-2D的重投影误差来获得位姿。
        Optimizer::PoseOptimization(&mCurrentFrame);

        // 步骤5 位姿优化后剔除outlier匹配点。
        int nmatchesMap = 0;
        for (int i = 0; i < mCurrentFrame.N; i++)
        {
            if (mCurrentFrame.mvpMapPoints[i])
            {
                if (mCurrentFrame.mvbOutlier[i])
                {
                    MapPoint *pMP = mCurrentFrame.mvpMapPoints[i];

                    mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint *>(NULL);
                    mCurrentFrame.mvbOutlier[i] = false;
                    pMP->mbTrackInView = false;
                    pMP->mnLastFrameSeen = mCurrentFrame.mnId;
                    nmatches--;
                }

                else if (mCurrentFrame.mvpMapPoints[i]->Observations() > 0)
                    nmatchesMap++;
            }
        }

        return nmatchesMap >= 10;
    }


    /**
     * @brief 双目或rgbd摄像头根据深度值为上一帧产生新的MapPoints
     *
     * 在双目和rgbd情况下，选取一些深度小一些的点（可靠一些） \n
     * 可以通过深度值产生一些新的MapPoints
     */
    void Tracking::UpdateLastFrame()
    {
        // Update pose according to reference keyframe
        // 步骤1：更新最近一帧的位姿
        KeyFrame *pRef = mLastFrame.mpReferenceKF;
        cv::Mat Tlr = mlRelativeFramePoses.back();

        mLastFrame.SetPose(Tlr * pRef->GetPose()); // Tlr*Trw = Tlw 1:last r:reference w:world

        // 如果上一帧为关键帧，或者单目的情况，则退出
        //if(mnLastKeyFrameId==mLastFrame.mnId || mSensor==System::MONOCULAR)
        if (mnLastKeyFrameId == mLastFrame.mnId || mSensor == System::MONOCULAR || !mbOnlyTracking)
            return;

        // 步骤2：对于双目或rgbd摄像头，为上一帧临时生成新的MapPoints
        // 注意这些MapPoints不加入到Map中，在tracking的最后会删除
        // 跟踪过程中需要将将上一帧的MapPoints投影到当前帧可以缩小匹配范围，加快当前帧与上一帧进行特征点匹配

        // Create "visual odometry" MapPoints
        // We sort points according to their measured depth by the stereo/RGB-D sensor
        // 步骤2.1：得到上一帧有深度值的特征点
        vector<pair<float, int> > vDepthIdx;
        vDepthIdx.reserve(mLastFrame.N);

        for (int i = 0; i < mLastFrame.N; i++)
        {
            float z = mLastFrame.mvDepth[i];
            if (z > 0)
            {
                vDepthIdx.push_back(make_pair(z, i));
            }
        }

        if (vDepthIdx.empty())
            return;

        // 步骤2.2：按照深度从小到大排序
        sort(vDepthIdx.begin(), vDepthIdx.end());

        // We insert all close points (depth<mThDepth)
        // If less than 100 close points, we insert the 100 closest ones.
        // 步骤2.3：将距离比较近的点包装成MapPoints
        int nPoints = 0;
        for (size_t j = 0; j < vDepthIdx.size(); j++)
        {
            int i = vDepthIdx[j].second;

            bool bCreateNew = false;

            MapPoint *pMP = mLastFrame.mvpMapPoints[i];
            if (!pMP)
                bCreateNew = true;
            else if (pMP->Observations() < 1)
            {
                bCreateNew = true;
            }

            if (bCreateNew)
            {
                // 这些生成MapPoints后并没有通过：
                // a.AddMapPoint、
                // b.AddObservation、
                // c.ComputeDistinctiveDescriptors、
                // d.UpdateNormalAndDepth添加属性，
                // 这些MapPoint仅仅为了提高双目和RGBD的跟踪成功率
                cv::Mat x3D = mLastFrame.UnprojectStereo(i);
                MapPoint *pNewMP = new MapPoint(x3D, mpMap, &mLastFrame, i);

                mLastFrame.mvpMapPoints[i] = pNewMP; // 添加新的MapPoint

                // 标记为临时添加的MapPoint，之后在CreateNewKeyFrame之前会全部删除
                mlpTemporalPoints.push_back(pNewMP);
                nPoints++;
            }
            else
            {
                nPoints++;
            }

            if (vDepthIdx[j].first > mThDepth && nPoints > 100)
                break;
        }
    }


    // 根据匀速模型对上一帧的MapPoints进行跟踪。
    // 1. 非单目情况下，需要对上一帧产生一些新的MapPoints。
    // 2. 将上一帧的MapPoints投影到当前帧的图像平面上，在投影的位置进行区域匹配。
    // 3. 根据匹配对估计当前帧的位姿。
    // 4. 根据位姿剔除误匹配。
    // return 如果匹配数目大于10，返回true。
    bool Tracking::TrackWithMotionModel()
    {

        ORBmatcher matcher(0.9, true);

        // 步骤1 对于双目或RGB-D，根据深度值维上一帧关键帧生成新的MapPoints。
        // (跟踪过程当前帧与上一帧进行特征点匹配，将上一帧的MapPoints投影到当前帧可以缩小匹配范围)。
        // 在跟踪过程中，剔除Outlier的MapPoint,不及时增加MapPoint会逐渐减少。
        UpdateLastFrame();          // 对于双目和RGB-D，增加上一帧的MapPoint数目。单目直接跳过函数。

        // 根据恒速模型估计当前帧位姿。
        mCurrentFrame.SetPose(mVelocity * mLastFrame.mTcw);

        // 初始化当前帧的MP。
        fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint *>(NULL));

        // 被之前帧看到的投影点搜索阈值。
        int th;
        if (mSensor != System::STEREO)
            th = 15;
        else
            th = 7;

        // 步骤2 根据匀速模型对上一帧的MapPoint进行跟踪。
        // 将上一帧的特征点对应的3D点投影到当前帧中，投影位置缩小匹配范围，通过描述子匹配。
        int nmatches = matcher.SearchByProjection(mCurrentFrame, mLastFrame, th, mSensor == System::MONOCULAR);

        // 如果匹配的点太少，扩大搜索半径再来一次。
        if (nmatches < 20)
        {
            fill(mCurrentFrame.mvpMapPoints.begin(), mCurrentFrame.mvpMapPoints.end(), static_cast<MapPoint *>(NULL));
            nmatches = matcher.SearchByProjection(mCurrentFrame, mLastFrame, 2 * th, mSensor == System::MONOCULAR);
        }

        if (nmatches < 20)
            return false;

        // 步骤3 BA-PnP求解当前帧位姿。
        Optimizer::PoseOptimization(&mCurrentFrame);

        // 步骤4 优化位姿后剔除mvpMapPoints中的离群值。
        int nmatchesMap = 0;
        for (int i = 0; i < mCurrentFrame.N; i++)
        {
            if (mCurrentFrame.mvpMapPoints[i])
            {
                if (mCurrentFrame.mvbOutlier[i])
                {
                    MapPoint *pMP = mCurrentFrame.mvpMapPoints[i];

                    mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint *>(NULL);
                    mCurrentFrame.mvbOutlier[i] = false;
                    pMP->mbTrackInView = false;
                    pMP->mnLastFrameSeen = mCurrentFrame.mnId;
                    nmatches--;
                }
                else if (mCurrentFrame.mvpMapPoints[i]->Observations() > 0)
                    nmatchesMap++;
            }
        }

        // 定位模式
        if (mbOnlyTracking)
        {
            mbVO = nmatchesMap < 10;
            return nmatches > 20;
        }

        return nmatchesMap >= 10;
    }


    // 对Local Map的MapPoints进行跟踪。
    // 1. 更新局部地图，包括局部关键帧和关键点。
    // 2. 对局部MapPoints进行投影匹配。
    // 3. 根据匹配对估计当前帧的姿态。
    // 4. 根据姿态剔除误匹配。
    // return 如果成功，true
    bool Tracking::TrackLocalMap()
    {
        // 步骤1 更新局部关键帧mvpLocalKeyFrames和局部地图点mvpLocalMapPoints
        UpdateLocalMap();

        // 步骤2 匹配局部地图与当前帧的MapPoint
        SearchLocalPoints();

        // 步骤3  局部的MapPoints与当前帧特征匹配后吗，PnP-BA对当前帧位姿进行更新，初始位姿在帧间跟踪已经给出了。
        // 之前，Relocalization、TrackReferenceKeyFrame、TrackWithMotionModel都有位姿优化。
        Optimizer::PoseOptimization(&mCurrentFrame);
        mnMatchesInliers = 0;                       // 当前帧跟踪到的MapPoints，用于统计跟踪效果。

        // 步骤4 更新当前帧MapPoints的被观测程度，统计跟踪局部地图的效果。
        for (int i = 0; i < mCurrentFrame.N; i++)
        {
            if (mCurrentFrame.mvpMapPoints[i])
            {
                // 当前帧的MapPoints被当前帧观测，观测统计量+1
                if (!mCurrentFrame.mvbOutlier[i])
                {
                    mCurrentFrame.mvpMapPoints[i]->IncreaseFound();
                    // slam模式
                    if (!mbOnlyTracking)
                    {
                        // 该MapPoint被其他关键帧观测到过，认为是内点。
                        if (mCurrentFrame.mvpMapPoints[i]->Observations() > 0)
                            mnMatchesInliers++;
                    }
                    else
                        // 记录当前帧跟踪到的MapPoints
                        mnMatchesInliers++;
                }
                else if (mSensor == System::STEREO)
                    mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint *>(NULL);
            }
        }

        // 步骤5 决定是否跟踪成功。
        if (mCurrentFrame.mnId < mnLastRelocFrameId + mMaxFrames && mnMatchesInliers < 50)
            return false;

        if (mnMatchesInliers < 30)
            return false;
        else
            return true;
    }


    // 66666，修改
    // 判断当前帧是否维关键帧,bLocalMappingIdle表示LocalMapping是否繁忙。
    // return 需要为true
    bool Tracking::NeedNewKeyFrame()
    {
        // 步骤1 如果用户选择定位模式，不插入关键帧。
        // 选择定位模式时，点云和关键帧都不会增加。
        if (mbOnlyTracking)
            return false;

        // 正在进行初始化状态更新，不添加新KF
        if (mpLocalMapper->GetUpdatingInitPoses())
        {
            cerr << "mpLocalMapper->GetUpdatingInitPoses, no new KF" << endl;
            return false;
        }
        // 如果局部地图被闭环检测使用，不插入关键帧。
        if (mpLocalMapper->isStopped() || mpLocalMapper->stopRequested())
            return false;

        const int nKFs = mpMap->KeyFramesInMap();       // 关键帧的数量。

        // 步骤2 判断距离上一次关键帧的时间。
        // mCurrentFrame.mnID时当前帧ID
        // mnLastRelocFrameId时最近一次重定位帧的ID
        // mMaxFrames等于输入图像的帧率。
        // 关键帧比较少，考虑插入关键帧。
        // 距离上次重定位超过1s(帧率),插入关键帧。
        if (mCurrentFrame.mnId < mnLastRelocFrameId + mMaxFrames && nKFs > mMaxFrames)
            return false;

        if (mbRelocBiasPrepare/* && mpLocalMapper->GetVINSInited()*/)
            return false;

        // 步骤3 得到参考帧跟踪到的MapPoints数量。
        // 在UpdateLocalKeyFrames函数中会将与当前帧共视程度最高的关键帧设定当前帧的参考管关键帧。
        int nMinObs = 3;            // 表示关键帧质量，越大表示被越多关键帧观测到。
        if (nKFs <= 2)
            nMinObs = 2;
        int nRefMatches = mpReferenceKF->TrackedMapPoints(nMinObs);

        // 步骤4 查询局部地图管理器是否繁忙。
        bool bLocalMappingIdle = mpLocalMapper->AcceptKeyFrames();

        // 步骤5 对于双目或RGBD摄像头，统计总的可以添加的MapPoints数量和在地图中跟踪到的MapPoints数量。
        int nMap = 0;
        int nTotal = 0;
        if (mSensor != System::MONOCULAR)      // 双目或RGB
        {
            for (int i = 0; i < mCurrentFrame.N; i++)
            {
                if (mCurrentFrame.mvDepth[i] > 0 && mCurrentFrame.mvDepth[i] < mThDepth)
                {
                    nTotal++;   // 可以添加MapPoints的总数。
                    if (mCurrentFrame.mvpMapPoints[i])
                        if (mCurrentFrame.mvpMapPoints[i]->Observations() > 0)
                            nMap++;     // 被关键帧观测到的点云数。
                }
            }
        }
        else
        {
            // 在单目中不存在视觉里程计匹配。
            nMap = 1;
            nTotal = 1;
        }

        const float ratioMap = (float) nMap / (float) (std::max(1, nTotal));

        // 步骤6 判断是否插入关键帧。

        // 设定inlier阈值和之前特征点匹配的inlier比例。
        float thRefRatio = 0.75f;
        if (nKFs < 2)
            thRefRatio = 0.4f;      // 只有一帧关键帧，插入关键帧的阈值很低。
        if (mSensor == System::MONOCULAR)
            thRefRatio = 0.9f;

        // MapPoints中和地图质量有关的比例阈值。
        float thMapRatio = 0.35f;
        if (mnMatchesInliers > 300)
            thMapRatio = 0.2f;

        // VI SLAM
        // 当前帧与LastKF的时间差
        double timegap = 0.1;
        if (mpLocalMapper->GetVINSInited())
            timegap = 0.5;

        const bool cTimeGap =
                ((mCurrentFrame.mTimeStamp - mpLastKeyFrame->mTimeStamp) >= timegap) && bLocalMappingIdle &&
                mnMatchesInliers > 15;


        // 关键帧判据
        // Condition 1a: More than "MaxFrames" have passed from last keyframe insertion
        // 长时间没有插入关键帧标志位, 超过1s。
        // const bool c1a = mCurrentFrame.mnId >= mnLastKeyFrameId+mMaxFrames;
        const bool c1a = mCurrentFrame.mTimeStamp >= mpLastKeyFrame->mTimeStamp + 3.0;
        // Condition 1b: More than "MinFrames" have passed and Local Mapping is idle
        // 等待插入关键帧的时间超过最短时间阈值，且localmapping处于空闲状态。
        const bool c1b = (mCurrentFrame.mnId >= mnLastKeyFrameId + mMinFrames && bLocalMappingIdle);
        // Condition 1c: tracking is weak
        // 要GG了，0.25和0.3是比较低的阈值。
        const bool c1c = mSensor != System::MONOCULAR && (mnMatchesInliers < nRefMatches * 0.25 || ratioMap < 0.3f);
        // Condition 2: Few tracked points compared to reference keyframe. Lots of visual odometry compared to map matches.
        // 阈值比c1c要高，与之前参考帧(最近的关键帧)重复度不是太高。
        const bool c2 = ((mnMatchesInliers < nRefMatches * thRefRatio || ratioMap < thMapRatio) &&
                         mnMatchesInliers > 15);

        // 满足以c2和c1a,c1b,c1c中的任何一个。
        if (((c1a || c1b || c1c) && c2) || cTimeGap)
        {
            // 局部地图构建线程空闲，是关键帧。
            if (bLocalMappingIdle)
                return true;

                // 局部地图构建线程忙碌，终止BA。
            else
            {
                // 双目和RGBD可以中断BA,单目只有空闲时才可以插入。
                mpLocalMapper->InterruptBA();
                if (mSensor != System::MONOCULAR)
                {
                    // 队列里不能阻塞太多关键帧。
                    // Tracking线程将关键帧先插入到mlNewKeyFrames中。
                    // 然后localmapper再逐个插入到mspKeyFrames中。
                    if (mpLocalMapper->KeyframesInQueue() < 3)
                        return true;
                    else
                        return false;
                }
                else
                    return false;
            }
        }
        else
            return false;
    }


    // 创建新的关键帧。对于非单目，同时创建MapPoints
    void Tracking::CreateNewKeyFrame()
    {
        // 局部地图不空闲，跳过。
        if (!mpLocalMapper->SetNotStop(true))
            return;

        // 步骤1 将当前帧构造成关键帧。
        // KeyFrame *pKF = new KeyFrame(mCurrentFrame, mpMap, mpKeyFrameDB);
        KeyFrame *pKF = new KeyFrame(mCurrentFrame, mpMap, mpKeyFrameDB, mvIMUSinceLastKF, mpLastKeyFrame);

        // 设置KF的初始导航状态
        pKF->SetInitialNavStateAndBias(mCurrentFrame.GetNavState());

        // 计算preintegrator
        pKF->ComputePreInt();

        // 清空上一帧关键帧到当前帧的IMU数据
        mvIMUSinceLastKF.clear();

        // 步骤2 将当前关键帧设置为之后帧的参考关键帧。
        // 在UpdateLocalKeyFrames函数中将与当前帧共视程度最高的关键帧设定为当前帧的参考关键帧。
        mpReferenceKF = pKF;                // Tracking 线程的参考关键帧是新建立的关键帧。
        mCurrentFrame.mpReferenceKF = pKF;

        // 步骤3 对于双目摄像头或RGB-D，为当前帧生成新的MapPoints。
        if (mSensor != System::MONOCULAR)
        {
            // 根据Tcw计算mRcw、mtcw和mRwc、mOw
            mCurrentFrame.UpdatePoseMatrices();

            // We sort points by the measured depth by the stereo/RGBD sensor.
            // We create all those MapPoints whose depth < mThDepth.
            // If there are less than 100 close points we create the 100 closest.
            // 步骤3.1：得到当前帧深度小于阈值的特征点
            // 创建新的MapPoint, depth < mThDepth
            vector<pair<float, int> > vDepthIdx;
            vDepthIdx.reserve(mCurrentFrame.N);
            for (int i = 0; i < mCurrentFrame.N; i++)
            {
                float z = mCurrentFrame.mvDepth[i];
                if (z > 0)
                {
                    vDepthIdx.push_back(make_pair(z, i));
                }
            }

            if (!vDepthIdx.empty())
            {
                // 步骤3.2：按照深度从小到大排序
                sort(vDepthIdx.begin(), vDepthIdx.end());

                // 步骤3.3：将距离比较近的点包装成MapPoints
                int nPoints = 0;
                for (size_t j = 0; j < vDepthIdx.size(); j++)
                {
                    int i = vDepthIdx[j].second;

                    bool bCreateNew = false;

                    MapPoint *pMP = mCurrentFrame.mvpMapPoints[i];
                    if (!pMP)
                        bCreateNew = true;
                    else if (pMP->Observations() < 1)
                    {
                        bCreateNew = true;
                        mCurrentFrame.mvpMapPoints[i] = static_cast<MapPoint *>(NULL);
                    }

                    if (bCreateNew)
                    {
                        cv::Mat x3D = mCurrentFrame.UnprojectStereo(i);
                        MapPoint *pNewMP = new MapPoint(x3D, pKF, mpMap);
                        // 这些添加属性的操作是每次创建MapPoint后都要做的
                        pNewMP->AddObservation(pKF, i);
                        pKF->AddMapPoint(pNewMP, i);
                        pNewMP->ComputeDistinctiveDescriptors();
                        pNewMP->UpdateNormalAndDepth();
                        mpMap->AddMapPoint(pNewMP);

                        mCurrentFrame.mvpMapPoints[i] = pNewMP;
                        nPoints++;
                    }
                    else
                    {
                        nPoints++;
                    }

                    // 这里决定了双目和rgbd摄像头时地图点云的稠密程度
                    // 但是仅仅为了让地图稠密直接改这些不太好，
                    // 因为这些MapPoints会参与之后整个slam过程
                    if (vDepthIdx[j].first > mThDepth && nPoints > 100)
                        break;
                }
            }
        }

        mpLocalMapper->InsertKeyFrame(pKF);

        mpLocalMapper->SetNotStop(false);

        mnLastKeyFrameId = mCurrentFrame.mnId;
        mpLastKeyFrame = pKF;
    }


    // 将局部地图中的地图点投影到当前帧，匹配点加入到mvpMapPoints中，但没有添加观测关系。
    // 在局部地图中查找当前帧视野范围内的点，将视野范围内点与当前帧的特征点进行投影匹配。
    void Tracking::SearchLocalPoints()
    {
        // 步骤1 遍历当前帧的地图点mvpMapPoints，标记这些MapPoint不参与之后的搜索。
        for (vector<MapPoint *>::iterator vit = mCurrentFrame.mvpMapPoints.begin(), vend = mCurrentFrame.mvpMapPoints.end();
             vit != vend; vit++)
        {
            MapPoint *pMP = *vit;
            if (pMP)
            {
                if (pMP->isBad())
                {
                    *vit = static_cast<MapPoint *>(NULL);
                }
                else
                {
                    // 能观测到改点的帧数+1
                    pMP->IncreaseVisible();
                    // 标记该点被当前帧观测。
                    pMP->mnLastFrameSeen = mCurrentFrame.mnId;
                    // 标记该点将来不进行投影，因为已经匹配。
                    pMP->mbTrackInView = false;
                }
            }
        }

        int nToMatch = 0;

        // 步骤2 将所有局部MapPoints投影到当前帧中，判断是否在视野范围内，进行投影匹配。
        for (vector<MapPoint *>::iterator vit = mvpLocalMapPoints.begin(), vend = mvpLocalMapPoints.end();
             vit != vend; vit++)
        {
            MapPoint *pMP = *vit;

            // 已经被当前帧观测到MapPoint不再判断是否被当前帧观测。
            if (pMP->mnLastFrameSeen == mCurrentFrame.mnId)
                continue;
            if (pMP->isBad())
                continue;

            // 步骤2.1 判断LocalMapPoints中的点是否在视野内。
            if (mCurrentFrame.isInFrustum(pMP, 0.5))
            {
                // 观测到该点的帧数+1，该点在视野内。
                pMP->IncreaseVisible();
                // 在视野内的MapPoint参与之后的投影匹配。
                nToMatch++;
            }
        }

        if (nToMatch > 0)
        {
            ORBmatcher matcher(0.8);
            int th = 1;
            if (mSensor == System::RGBD)
                th = 3;

            // 如果不久之前重定位过，进行更宽范围的搜索，阈值加大。
            if (mCurrentFrame.mnId < mnLastRelocFrameId + 2)
                th = 5;

            // 步骤2.2 对视野范围内的MapPoint通过投影进行特征点匹配，匹配的点云添加在当前帧MP中。
            matcher.SearchByProjection(mCurrentFrame, mvpLocalMapPoints, th);
        }
    }


    // 更新LocalMap， 提取当前帧下的局部地图
    // 局部地图包括，K1个关键帧（包含参考关键帧），K2个临近关键帧，这些关键帧观测到的MapPoints。
    void Tracking::UpdateLocalMap()
    {
        // 设置可视化显示的参考点云。
        mpMap->SetReferenceMapPoints(mvpLocalMapPoints);

        // 更新局部关键帧和局部点云。
        UpdateLocalKeyFrames();
        UpdateLocalPoints();
    }


    // 更新局部地图点云，提取局部地图中的点云。
    // 不同的当前帧不同的局部地图。
    // 利用局部关键mvpLocalKeyFrames的MapPoints更新mvpLocalMapPoints
    void Tracking::UpdateLocalPoints()
    {

        // 步骤1 清空局部MapPoint
        mvpLocalMapPoints.clear();

        // 步骤2 遍历局部关键帧mvpLocalKeyFrames,进行更新。
        for (vector<KeyFrame *>::const_iterator itKF = mvpLocalKeyFrames.begin(), itEndKF = mvpLocalKeyFrames.end();
             itKF != itEndKF; itKF++)
        {

            KeyFrame *pKF = *itKF;
            const vector<MapPoint *> vpMPs = pKF->GetMapPointMatches();

            // 步骤2 将局部关键帧的MapPoints添加到mvpLocalMapPoints。
            for (vector<MapPoint *>::const_iterator itMP = vpMPs.begin(), itEndMP = vpMPs.end();
                 itMP != itEndMP; itMP++)
            {
                MapPoint *pMP = *itMP;
                if (!pMP)
                    continue;
                // 防止重复添加局部MapPoints
                if (pMP->mnTrackReferenceForFrame == mCurrentFrame.mnId)
                    continue;
                if (!pMP->isBad())
                {
                    mvpLocalMapPoints.push_back(pMP);
                    pMP->mnTrackReferenceForFrame = mCurrentFrame.mnId;
                }
            }
        }
    }


    // 更新局部关键帧，提取局部地图中的关键帧
    // 不同的当前帧，对应不同的局部关键帧。
    // 遍历当前帧的MapPoints，将观测到这些MapPoint的关键帧和相邻帧取出，更新mvpLocalKeyFrames
    void Tracking::UpdateLocalKeyFrames()
    {

        // 步骤1 遍历当前帧的所有MapPoints，记录所有能观测到当前帧MapPoints的关键帧。
        map<KeyFrame *, int> keyframeCounter;
        for (int i = 0; i < mCurrentFrame.N; i++)
        {
            if (mCurrentFrame.mvpMapPoints[i])
            {
                MapPoint *pMP = mCurrentFrame.mvpMapPoints[i];
                if (!pMP->isBad())
                {
                    // 能观测到当前帧MapPoints的关键帧。
                    const mapMapPointObs/*map<KeyFrame*, size_t>*/ observations = pMP->GetObservations();
                    for (mapMapPointObs/*map<KeyFrame*, size_t>*/::const_iterator it = observations.begin(), itend = observations.end();
                         it != itend; it++)
                        keyframeCounter[it->first]++;
                }
                else
                {
                    mCurrentFrame.mvpMapPoints[i] = NULL;
                }
            }
        }

        if (keyframeCounter.empty())
            return;

        int max = 0;
        KeyFrame *pKFmax = static_cast<KeyFrame *>(NULL);

        // 步骤2 更新局部关键帧(mvpLocalKeyFrames)

        // 清空之前的局部关键帧，分配内存空间。
        mvpLocalKeyFrames.clear();
        mvpLocalKeyFrames.reserve(3 * keyframeCounter.size());

        // 步骤2.1 能观测到当前帧地图点云的关键帧作为局部关键帧。
        for (map<KeyFrame *, int>::const_iterator it = keyframeCounter.begin(), itEnd = keyframeCounter.end();
             it != itEnd; it++)
        {
            KeyFrame *pKF = it->first;
            if (pKF->isBad())
                continue;

            // 其中最多共视连接的关键帧。
            if (it->second > max)
            {
                max = it->second;
                pKFmax = pKF;
            }

            // 添加局部关键帧。
            mvpLocalKeyFrames.push_back(it->first);
            // 防止冲突添加局部关键帧。
            pKF->mnTrackReferenceForFrame = mCurrentFrame.mnId;

        }   // 步骤2.1

        // 步骤2.2 与策略1得到的局部关键帧共视程度很高的关键很作为局部关键帧。
        for (vector<KeyFrame *>::const_iterator itKF = mvpLocalKeyFrames.begin(), itEndKF = mvpLocalKeyFrames.end();
             itKF != itEndKF; itKF++)
        {
            // 限制关键帧数量。
            if (mvpLocalKeyFrames.size() > 80)
                break;

            KeyFrame *pKF = *itKF;

            // a.与pKF最佳共视的10帧。
            const vector<KeyFrame *> vNeighs = pKF->GetBestCovisibilityKeyFrames(10);
            for (vector<KeyFrame *>::const_iterator itNeighKF = vNeighs.begin(), itEndNeighKF = vNeighs.end();
                 itNeighKF != itEndNeighKF; itNeighKF++)
            {
                KeyFrame *pNeighKF = *itNeighKF;
                if (!pNeighKF->isBad())
                {
                    // 在步骤2.1中添加过的，不添加了。
                    if (pNeighKF->mnTrackReferenceForFrame != mCurrentFrame.mnId)
                    {
                        mvpLocalKeyFrames.push_back(pNeighKF);
                        pNeighKF->mnTrackReferenceForFrame = mCurrentFrame.mnId;  // mnTrackReferenceForFrame标志为，被mnId赋值表示这一帧已经添加过。
                        break;      // 只要最高共视的关键帧，之后的共视都不添加了。
                    }
                }
            }

            // b.自己的子关键帧,Spanning tree有关。
            const set<KeyFrame *> spChilds = pKF->GetChilds();
            for (set<KeyFrame *>::const_iterator sit = spChilds.begin(), send = spChilds.end(); sit != send; sit++)
            {
                KeyFrame *pChildKF = *sit;
                if (!pChildKF->isBad())
                {
                    // 防止重复添加
                    if (pChildKF->mnTrackReferenceForFrame != mCurrentFrame.mnId)
                    {
                        mvpLocalKeyFrames.push_back(pChildKF);
                        // 设定标志位，表示已添加。
                        pChildKF->mnTrackReferenceForFrame = mCurrentFrame.mnId;
                        break;
                    }
                }
            }

            // c.自己的父关键帧，和spanning tree有关。
            KeyFrame *pParent = pKF->GetParent();
            if (pParent)
            {
                // 防止重复添加
                if (pParent->mnTrackReferenceForFrame != mCurrentFrame.mnId)
                {
                    mvpLocalKeyFrames.push_back(pParent);
                    pParent->mnTrackReferenceForFrame = mCurrentFrame.mnId;
                    // break;
                }
            }


            KeyFrame *pPrevKF = pKF->GetPrevKeyFrame();
            if (pPrevKF)
            {
                if (pPrevKF->isBad())
                    cerr << "pPrevKF is bad in UpdateLocalKeyFrames()?????" << endl;
                if (pPrevKF->mnTrackReferenceForFrame != mCurrentFrame.mnId)
                {
                    mvpLocalKeyFrames.push_back(pPrevKF);
                    pPrevKF->mnTrackReferenceForFrame = mCurrentFrame.mnId;
                }
            }

            KeyFrame *pNextKF = pKF->GetNextKeyFrame();
            if (pNextKF)
            {
                if (pNextKF->isBad())
                    cerr << "pNextKF is bad in UpdateLocalKeyFrames()?????" << endl;
                if (pNextKF->mnTrackReferenceForFrame != mCurrentFrame.mnId)
                {
                    mvpLocalKeyFrames.push_back(pNextKF);
                    pNextKF->mnTrackReferenceForFrame = mCurrentFrame.mnId;
                }
            }


        }   // 步骤2.2

        // 步骤3 更新当前帧的参考关键帧，与当前帧共视程度最高的帧作为参考关键帧。
        if (pKFmax)
        {
            mpReferenceKF = pKFmax;
            mCurrentFrame.mpReferenceKF = mpReferenceKF;
        }

    }


    // 重定位。
    // 位姿计算采用EPnP
    bool Tracking::Relocalization()
    {

        // 步骤1 计算当前帧特征点的BoW
        mCurrentFrame.ComputeBoW();

        // 步骤2 找到与当前帧相似的候选关键帧。
        vector<KeyFrame *> vpCandidateKFs = mpKeyFrameDB->DetectRelocalizationCandidates(&mCurrentFrame);

        if (vpCandidateKFs.empty())
            return false;

        const int nKFs = vpCandidateKFs.size();     // 候选帧数量。

        // 首先和每一个候选关键帧进行ORB特征匹配。
        // 如果有足够的匹配点，设置PnP求解器。
        // 定义匹配和PnP求解器对象。
        ORBmatcher matcher(0.75, true);

        vector<PnPsolver *> vpPnPsolvers;
        vpPnPsolvers.resize(nKFs);

        // 某1帧的匹配点云数组。
        vector<vector<MapPoint *>> vvpMapPointMatches;
        vvpMapPointMatches.resize(nKFs);

        vector<bool> vbDiscarded;
        vbDiscarded.resize(nKFs);
        // 通过BoW匹配的关键帧个数。
        int nCandidates = 0;

        for (int i = 0; i < nKFs; i++)
        {
            KeyFrame *pKF = vpCandidateKFs[i];
            // 剔除不好的候选关键帧。
            if (pKF->isBad())
                vbDiscarded[i] = true;
            else
            {
                // 步骤3 通过BoW进行匹配。
                //  int nmatches = matcher.SearchByBow(pKF, mCurrentFrame, vvpMapPointMatches[i]);
                int nmatches = matcher.SearchByBoW(pKF, mCurrentFrame, vvpMapPointMatches[i]);
                //int nmatches = matcher.SearchByBoW(pKF, mCurrentFrame, vvpMapPointMatches[i]);
                // 匹配点太少，剔除。
                if (nmatches < 15)
                {
                    vbDiscarded[i] = true;
                    continue;
                }
                    // 进行匹配PnP求解。
                else
                {
                    // 初始化PnP求解器。
                    PnPsolver *pSolver = new PnPsolver(mCurrentFrame, vvpMapPointMatches[i]);
                    pSolver->SetRansacParameters(0.99, 10, 300, 4, 0.5, 5.991);
                    vpPnPsolvers[i] = pSolver;
                    nCandidates++;
                }
            }
        }

        // 选择进行EPnP RANSAC迭代，直到相机位姿足够好。
        bool bMatch = false;
        ORBmatcher matcher2(0.9, true);

        while (nCandidates > 0 && !bMatch)
        {
            for (int i = 0; i < nKFs; i++)
            {
                // 剔除，跳过这个帧。
                if (vbDiscarded[i])
                    continue;

                // RANSAC迭代变量。
                vector<bool> vbInliers;
                int nInliers;
                bool bNoMore;

                // 步骤4 EPnP估算位姿。
                PnPsolver *pSolver = vpPnPsolvers[i];
                cv::Mat Tcw = pSolver->iterate(5, bNoMore, vbInliers, nInliers);

                // 如果RANSAC迭代达到最大次数，剔除迭代关键帧。
                if (bNoMore)
                {
                    vbDiscarded[i] = true;
                    nCandidates--;
                }

                // 如果计算得到相机位姿。
                if (!Tcw.empty())
                {
                    Tcw.copyTo(mCurrentFrame.mTcw);

                    set<MapPoint *> sFound;

                    const int np = vbInliers.size();

                    for (int j = 0; j < np; j++)
                    {
                        // 地图点时内点。
                        if (vbInliers[j])
                        {
                            // 当前帧特征点对应的MapPoint。
                            mCurrentFrame.mvpMapPoints[j] = vvpMapPointMatches[i][j];
                            sFound.insert(vvpMapPointMatches[i][j]);    // 匹配的点云。
                        }
                        else
                            mCurrentFrame.mvpMapPoints[j] = NULL;
                    }

                    // 步骤5 通过BA优化相机位姿。
                    int nGood = Optimizer::PoseOptimization(&mCurrentFrame);

                    if (nGood < 10)
                        continue;   // 跳过这一帧，不合格。

                    for (int io = 0; io < mCurrentFrame.N; io++)
                        // 剔除外点。
                        if (mCurrentFrame.mvbOutlier[io])
                            mCurrentFrame.mvpMapPoints[io] = static_cast<MapPoint *>(NULL);

                    // 步骤6 如果内点较少，通过投影方式对为匹配的点进行匹配，再优化求解。
                    if (nGood < 50)
                    {
                        int nadditional = matcher2.SearchByProjection(mCurrentFrame, vpCandidateKFs[i], sFound, 10,
                                                                      100);

                        if (nadditional + nGood >= 50)
                        {
                            nGood = Optimizer::PoseOptimization(&mCurrentFrame);

                            // 如果内点还是不足，在更小的窗口内再次进行投影匹配。
                            if (nGood > 30 && nGood < 50)
                            {
                                sFound.clear();
                                for (int ip = 0; ip < mCurrentFrame.N; ip++)
                                    if (mCurrentFrame.mvpMapPoints[ip])
                                        sFound.insert(mCurrentFrame.mvpMapPoints[ip]);
                                nadditional = matcher2.SearchByProjection(mCurrentFrame, vpCandidateKFs[i], sFound, 3,
                                                                          64);

                                // 最终优化。
                                if (nGood + nadditional >= 50)
                                {
                                    nGood = Optimizer::PoseOptimization(&mCurrentFrame);

                                    for (int io = 0; io < mCurrentFrame.N; io++)
                                        if (mCurrentFrame.mvbOutlier[io])
                                            mCurrentFrame.mvpMapPoints[io] = NULL;
                                }   // 最终优化。
                            }   // 内点数不足。
                        }   // 再次投影匹配后内点满足。
                    }   // 再次投影匹配。

                    // 如果通过足够的内点得到了位姿，停止RANSAC并继续。
                    if (nGood >= 50)
                    {
                        bMatch = true;  // 在KeyFrameDB中找到了重定位帧。
                        break; // 不找了。
                    }
                }   // 得到相机位姿。
            }   // for(int i=0; i<nKFs; i++)。
        }   // while(nCandidates>0 && !bMatch) 

        if (!bMatch)
            return false;
        else
        {
            //Test log
            if (!mpLocalMapper->GetVINSInited())
                cerr << "VINS not inited? why." << endl;

            mbRelocBiasPrepare = true;
            mnLastRelocFrameId = mCurrentFrame.mnId;
            return true;
        }
    }


    // 重置系统。
    void Tracking::Reset()
    {
        // 发送停止viewer请求
        mpViewer->RequestStop();

        cout << "System Reseting" << endl;
        // 等待终止viewer
        while (!mpViewer->isStopped())
        {
            // 延时3000us
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }

        // 重置局部地图线程。
        cout << "Reseting Local Mapper ...";
        mpLocalMapper->RequestReset();
        cout << "done" << endl;

        // 重置闭环检测线程。
        cout << "Reseting Loop Closing ... ";
        mpLoopClosing->RequestReset();
        cout << "done" << endl;

        // 清空BoW数据库。
        cout << "Reseting Database ... ";
        mpKeyFrameDB->clear();
        cout << "done" << endl;

        // 清空地图。
        mpMap->clear();

        KeyFrame::nNextId = 0;
        Frame::nNextId = 0;
        mState = NO_IMAGES_YET;

        if (mpInitializer)
        {
            delete mpInitializer;
            mpInitializer = static_cast<Initializer *>(NULL);
        }

        mlRelativeFramePoses.clear();
        mlpReferences.clear();
        mlFrameTimes.clear();
        mlbLost.clear();

        mpViewer->Release();

    }

    // 修改设置参数。
    void Tracking::ChangeCalibration(const string &strSettingPath)
    {

        cv::FileStorage fSettings(strSettingPath, cv::FileStorage::READ);
        float fx = fSettings["Camera.fx"];
        float fy = fSettings["Camera.fy"];
        float cx = fSettings["Camera.cx"];
        float cy = fSettings["Camera.cy"];

        cv::Mat K = cv::Mat::eye(3, 3, CV_32F);
        K.at<float>(0, 0) = fx;
        K.at<float>(1, 1) = fy;
        K.at<float>(0, 2) = cx;
        K.at<float>(1, 2) = cy;
        K.copyTo(mK);

        cv::Mat DistCoef(4, 1, CV_32F);
        DistCoef.at<float>(0) = fSettings["Camera.k1"];
        DistCoef.at<float>(1) = fSettings["Camera.k2"];
        DistCoef.at<float>(2) = fSettings["Camera.p1"];
        DistCoef.at<float>(3) = fSettings["Camera.p2"];
        const float k3 = fSettings["Camera.k3"];
        if (k3 != 0)
        {
            DistCoef.resize(5);
            DistCoef.at<float>(4) = k3;
        }
        DistCoef.copyTo(mDistCoef);

        mbf = fSettings["Camera.bf"];

        Frame::mbInitialComputations = true;

    }

    void Tracking::InformOnlyTracking(const bool &flag)
    {
        mbOnlyTracking = flag;
    }


}   // ORB_SLAM2


