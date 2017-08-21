//定义预处理变量，#ifndef variance_name 表示变量未定义时为真，并执行之后的代码直到遇到 #endif。
#ifndef LOOPCLOSING_H
#define LOOPCLOSING_H

#include "KeyFrame.h"
#include "LocalMapping.h"
#include "Map.h"
#include "ORBVocabulary.h"
#include "Tracking.h"
#include "KeyFrameDatabase.h"

#include <thread>
#include <mutex>
#include "Thirdparty/g2o/g2o/types/types_seven_dof_expmap.h"

#include "IMU/configparam.h"

namespace ORB_SLAM2
{

    class Tracking;

    class LocalMapping;

    class KeyFrameDatabase;


    class LoopClosing
    {
    public:

        // Vison+IMU
        ConfigParam *mpParams;

        bool GetMapUpdateFlagForTracking();

        void SetMapUpdateFlagInTracking(bool bflag);

    protected:
        std::mutex mMutexMapUpdateFlag;
        bool mbMapUpdateFlagForTracking;


    public:

        typedef pair<set<KeyFrame *>, int> ConsistentGroup;
        typedef map<KeyFrame *, g2o::Sim3, std::less<KeyFrame *>,
                Eigen::aligned_allocator<std::pair<const KeyFrame *, g2o::Sim3>>> KeyFrameAndPose;

    public:

        // 构造函数。
        // LoopClosing(Map *pMap, KeyFrameDatabase *pDB, ORBVocabulary *pVoc, const bool bFixScale);
        LoopClosing(Map *pMap, KeyFrameDatabase *pDB, ORBVocabulary *pVoc, const bool bFixScale, ConfigParam *pParams);

        // 设置跟踪线程对象指针。
        void SetTracker(Tracking *pTracker);

        //  设置局部地图线程对象指针。
        void SetLocalMapper(LocalMapping *pLocalMapper);

        // 主函数。
        void Run();

        void InsertKeyFrame(KeyFrame *pKF);

        void RequestReset();

        // 下面这些的全局BA运行在另一个独立的线程中。
        void RunGlobalBundleAdjustment(unsigned long nLoopKF);

        // 运行全局BA。
        bool isRunningGBA()
        {
            unique_lock<std::mutex> lock(mMutexGBA);
            return mbRunningGBA;
        }

        // 全局BA完成。
        bool isFinishedGBA()
        {
            unique_lock<std::mutex> lock(mMutexGBA);
            return mbFinishedGBA;
        }

        void RequestFinish();

        bool isFinished();

    protected:

        bool CheckNewKeyFrames();

        bool DetectLoop();

        bool ComputeSim3();

        void SearchAndFuse(const KeyFrameAndPose &CorrectedPosesMap);

        void CorrectLoop();

        void ResetIfRequested();

        bool mbResetRequested;
        std::mutex mMutexReset;

        bool CheckFinish();

        void SetFinish();

        bool mbFinishRequested;
        bool mbFinished;
        std::mutex mMutexFinish;

        Map *mpMap;
        Tracking *mpTracker;

        KeyFrameDatabase *mpKeyFrameDB;
        ORBVocabulary *mpORBVocabulary;

        LocalMapping *mpLocalMapper;

        std::list<KeyFrame *> mlpLoopKeyFrameQueue;

        std::mutex mMutexLoopQueue;

        // 闭环检测参数， 一致组中元素个数。
        float mnCovisibilityConsistencyTh;

        // 闭环检测变量。
        KeyFrame *mpCurrentKF;
        KeyFrame *mpMatchedKF;
        std::vector<ConsistentGroup> mvConsistentGroups;            // 一致组
        std::vector<KeyFrame *> mvpEnoughConsistentCandidates;        // 最终的闭环候选帧。
        std::vector<KeyFrame *> mvpCurrentConnectedKFs;                // 当前关键帧共视图。
        std::vector<MapPoint *> mvpCurrentMatchedPoints;            // 帧间sim3优化后的匹配内点。
        std::vector<MapPoint *> mvpLoopMapPoints;                    // 当前关键帧共视图点云。
        cv::Mat mScw;                                                // 当前关键帧的sim位姿(cv::Mat)。
        g2o::Sim3 mg2oScw;                                            // 当前关键帧sim3位姿(g2o)。

        long unsigned int mLastLoopKFid;

        // 全局BA变量。
        bool mbRunningGBA;
        bool mbFinishedGBA;
        bool mbStopGBA;
        std::mutex mMutexGBA;
        std::thread *mpThreadGBA;

        // 双目/RGB-D固定尺度
        bool mbFixScale;

        bool mnFullBAIdx;

    };

}


#endif
