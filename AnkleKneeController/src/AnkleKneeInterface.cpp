#include <AnkleKneeInterface/AnkleKneeInterface.hpp>
#include <utils/BSplineBasic.h>
#include <utils/ParameterFetcher.hpp>
#include <utils/utilities.hpp>
#include <cmath>
#include <data_manager/DataManager.hpp>
#include <nodelet/nodelet.h>

AnkleKneeInterface::AnkleKneeInterface() : mTime(0.0),
                                           mCount(0),
                                           mInitTime(0.01),
                                           mServoRate(1.0/1500),
                                           mNumJoint(2),
                                           mGrav(9.81) {

    mJPosAct.setZero();
    mJVelAct.setZero();
    mJEffAct.setZero();
    mJPosDes.setZero();
    mJVelDes.setZero();
    mJEffDes.setZero();
    DataManager::getDataManager()->RegisterData(&mTime, DOUBLE, "time", 1);
    DataManager::getDataManager()->RegisterData(&mJPosAct, VECT2, "JPosAct", 2);
    DataManager::getDataManager()->RegisterData(&mJVelAct, VECT2, "JVelAct", 2);
    DataManager::getDataManager()->RegisterData(&mJEffAct, VECT2, "JEffAct", 2);
    DataManager::getDataManager()->RegisterData(&mJPosDes, VECT2, "JPosDes", 2);
    DataManager::getDataManager()->RegisterData(&mJVelDes, VECT2, "JVelDes", 2);
    DataManager::getDataManager()->RegisterData(&mJEffDes, VECT2, "JEffDes", 2);

    ros::NodeHandle nh("/high_level_system/model/com");
    mA = Eigen::MatrixXd::Zero(2, 2);
    mMass = 0.0;
    mRx = 0.0;
    mRy = 0.0;
    ParameterFetcher::searchReqParam(nh, "mass", mMass);
    ParameterFetcher::searchReqParam(nh, "rx", mRx);
    ParameterFetcher::searchReqParam(nh, "ry", mRy);
    mA(0, 0) = mMass * mRx * mRx;

    ros::NodeHandle nh2("/high_level_system/planner/chirp/");
    m_high_freq = 300.;
    m_low_freq = 0.001;
    m_rate = 0.015;
    m_amp = 1.0;
    m_offset = 5.0;
    m_start_dur = 1.;
    m_end_dur = 0.5;
    m_save_input.reserve(5000000);
    m_save_output.reserve(5000000);
    m_save_time.reserve(5000000);
    m_is_saved = false;
    m_is_saved_cmd = false;
    m_is_current_chirp = false;
    ParameterFetcher::searchReqParam(nh2, "highFreq", m_high_freq);
    ParameterFetcher::searchReqParam(nh2, "lowFreq", m_low_freq);
    ParameterFetcher::searchReqParam(nh2, "rate", m_rate);
    ParameterFetcher::searchReqParam(nh2, "amp", m_amp);
    ParameterFetcher::searchReqParam(nh2, "offset", m_offset);
    ParameterFetcher::searchReqParam(nh2, "startDur", m_start_dur);
    ParameterFetcher::searchReqParam(nh2, "endDur", m_end_dur);
    ros::NodeHandle nh3("/high_level_system/env/mode/");
    ParameterFetcher::searchReqParam(nh3, "controlMode", mMode);
    if (mMode == 1) {
        m_is_current_chirp = true;
    } else {
        m_is_current_chirp = false;
    }
}

AnkleKneeInterface::~AnkleKneeInterface() {

}

void AnkleKneeInterface::getCommand(std::shared_ptr<AnkleKneeSensorData> data,
                                    std::shared_ptr<AnkleKneeCommand> cmd) {
    if (mTime < mInitTime) {
        _initialize(data, cmd);
    } else {
        //_maintainInitialPosition(data, cmd);
        //_sinusoidalPosition(data, cmd);
        _chirpSignal(data, cmd);
    }
    mTime += mServoRate;
    mCount += 1;
    mJPosAct = data->q;
    mJVelAct = data->qdot;
    mJEffAct = data->jtrq;
    mJPosDes = cmd->q;
    mJVelDes = cmd->qdot;
    mJEffDes = cmd->jtrq;
}

void AnkleKneeInterface::_initialize(std::shared_ptr<AnkleKneeSensorData> data,
                                     std::shared_ptr<AnkleKneeCommand> cmd) {
    if (mMode == 0) {
        cmd->q = data->q;
        mInitQ = data->q;
        cmd->jtrq.setZero();
        cmd->qdot.setZero();
        std::cout << "i am 0" << std::endl;
    } else if (mMode == 1) {
        cmd->qdot.setZero();
        std::cout << "i am 1" << std::endl;
    } else {
        cmd->q = data->chirp;
        std::cout << "i am 2" << std::endl;
    }
    DataManager::getDataManager()->start();
}

void AnkleKneeInterface::_chirpSignal(std::shared_ptr<AnkleKneeSensorData> data,
                                      std::shared_ptr<AnkleKneeCommand> cmd) {

    static bool isFirstSplineGen(false);
    static bool isLastSplineGen(false);
    static BS_Basic<1, 3, 0, 2, 2> firstSpline;
    static double firstSplineInitTime(0.0);
    static BS_Basic<1, 3, 0, 2, 2> lastSpline;
    static double lastSplineInitTime(0.0);
    static double d_ary[1];
    static Eigen::Vector2d chirpInit = data->chirp;
    double signal(0.0);
    double velocity(0.0);

    if (mTime < mInitTime + m_start_dur) {
        if (!isFirstSplineGen) {
            firstSplineInitTime = mTime;
            double ini[3] = {chirpInit[0], 0, 0};
            double fin[3] = {m_offset, 0, 0};
            double **middle_pt;
            firstSpline.SetParam(ini, fin, middle_pt, m_start_dur);
            isFirstSplineGen = true;
        }
        firstSpline.getCurvePoint(mTime - firstSplineInitTime, d_ary);
        signal = d_ary[0];
        if (m_is_current_chirp) {
            cmd->jtrq[0] = signal;
            cmd->jtrq[1] = 0.;
        } else {
            cmd->q[0] = signal;
            cmd->q[1] = chirpInit[1];
        }
    } else {
        static double prev_sample_time = 0;
        static double prev_effective_angle = 0;

        double elapsed_time = mTime - mInitTime - m_start_dur;
        double effective_angle, effective_switching_freq_hz;
        double range = m_high_freq - m_low_freq;
        double period = elapsed_time - prev_sample_time;
        prev_sample_time = elapsed_time;

        //exponential chirp
        effective_angle = 2 * M_PI * m_low_freq * (pow(m_rate * range, elapsed_time) - 1) / log(m_rate * range);
        //ROS_INFO("effective_angle:%f\t elapsed_time:%f", effective_angle, elapsed_time);

        signal = m_amp * sin(effective_angle) + m_offset;
        velocity = m_amp * cos(effective_angle) * ((2*M_PI*m_low_freq) / (log(m_rate*range))) * pow(m_rate * range, elapsed_time) * log(m_rate*range);

        if(period <= 0.0)
        {
            effective_switching_freq_hz = 0.0;
        }
        else
        {
            effective_switching_freq_hz = (effective_angle - prev_effective_angle) / period / 2 / M_PI;
        }
        prev_effective_angle = effective_angle;

        if(effective_switching_freq_hz > m_high_freq)
        {
            if (!isLastSplineGen) {
                lastSplineInitTime = mTime;
                double ini[3] = {m_last_signal, m_last_velocity, 0};
                double fin[3] = {chirpInit[0], 0, 0};
                double **middle_pt;
                lastSpline.SetParam(ini, fin, middle_pt, m_end_dur);
                isLastSplineGen = true;
            }
            if (mTime < lastSplineInitTime + m_end_dur) {
                lastSpline.getCurvePoint(mTime - lastSplineInitTime, d_ary);
                signal = d_ary[0];
            } else {
                signal = chirpInit[0];
            }
        }
        if (m_is_current_chirp) {
            cmd->jtrq[0] = signal;
            cmd->jtrq[1] = 0.0;
        } else {
            cmd->q[0] = signal;
            cmd->q[1] = chirpInit[1];
        }

        m_last_signal = signal;
        m_last_velocity = velocity;

        if(effective_switching_freq_hz < m_high_freq ) {
            m_save_input.push_back(data->chirpInput[0]);
            m_save_output.push_back(data->chirpOutput[0]);
            m_save_time.push_back(mTime);
        } else {
            if (!m_is_saved) {
                sejong::saveVector(m_save_input, "input");
                sejong::saveVector(m_save_output, "output");
                sejong::saveVector(m_save_time, "time");
                m_is_saved = true;
                std::cout << "DATA INPUT OUTPUT IS SAVED!" << std::endl;
            }
        }
    }
    // For debugging purpose
    if (mTime < 15.0) {
        m_save_command.push_back(signal);
    } else {
        if (!m_is_saved_cmd) {
            sejong::saveVector(m_save_command, "command");
            m_is_saved_cmd = true;
            std::cout << "DATA CMD IS SAVED!" << std::endl;
        }
    }
}

void AnkleKneeInterface::_maintainInitialPosition(std::shared_ptr<AnkleKneeSensorData> data,
                                                  std::shared_ptr<AnkleKneeCommand> cmd) {
    cmd->q = mInitQ;
    cmd->qdot.setZero();

    cmd->jtrq[0] = (mMass * mGrav * mRx * cos(M_PI - data->q[0]));
    cmd->jtrq[1] = 0.;
    //cmd->jtrq.setZero();
}

void AnkleKneeInterface::_sinusoidalPosition(std::shared_ptr<AnkleKneeSensorData> data,
                                             std::shared_ptr<AnkleKneeCommand> cmd) {
    static bool isFirst(true);
    static BS_Basic<2, 3, 0, 2, 2> spline;
    static double transDur(0.0);
    static double initTime(mTime);
    static std::vector<double> mid(2);
    static std::vector<double> amp(2);
    static std::vector<double> freq(2);
    static double d_ary[2];
    if (isFirst) {
        // Do Plan
        ros::NodeHandle nh("/high_level_system/planner/sinusoidal");
        ParameterFetcher::searchReqParam(nh, "transDur", transDur);
        ParameterFetcher::searchReqParam(nh, "mid", mid);
        ParameterFetcher::searchReqParam(nh, "amp", amp);
        ParameterFetcher::searchReqParam(nh, "freq", freq);
        double ini[6] = {data->q[0], data->q[1], 0., 0., 0., 0.};
        double fin[6] = {mid[0], mid[1], amp[0]*2*M_PI*freq[0], amp[1]*2*M_PI*freq[1], 0., 0.};
        double **middle_pt;
        spline.SetParam(ini, fin, middle_pt, transDur);
        isFirst = false;
    } else {
        // Get Trajectory
        if (mTime < initTime + transDur) {
            spline.getCurvePoint(mTime - initTime, d_ary);
            for (int i = 0; i < 2; ++i) cmd->q[i] = d_ary[i];
            spline.getCurveDerPoint(mTime - initTime, 1, d_ary);
            for (int i = 0; i < 2; ++i) cmd->qdot[i] = d_ary[i];
            spline.getCurveDerPoint(mTime - initTime, 2, d_ary);
            for (int i = 0; i < 2; ++i) cmd->jtrq[i] = d_ary[i];
            cmd->jtrq = mA * cmd->jtrq; // feedforword
            cmd->jtrq[0] = cmd->jtrq[0] + (mMass * mGrav * mRx * cos(M_PI - data->q[0])); // gravity
            //cmd->jtrq.setZero();
        } else {
            for (int i = 0; i < 2; ++i) {
                cmd->q[i] =
                    amp[i] * sin(2*M_PI*freq[i]* (mTime - transDur)) + mid[i];
                cmd->qdot[i] =
                    amp[i]*2*M_PI*freq[i]*cos(2*M_PI*freq[i]*(mTime - transDur));
                cmd->jtrq[i] =
                    -amp[i]*2*M_PI*freq[i]*2*M_PI*freq[i]*sin(2*M_PI*freq[i]*(mTime - transDur));
            }
            cmd->jtrq = mA*cmd->jtrq; // feedforword
            cmd->jtrq[0] = cmd->jtrq[0] + (mMass * mGrav * mRx * cos(M_PI - data->q[0])); // gravity
            //cmd->jtrq.setZero();
        }
    }
}
