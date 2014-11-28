#define NDEBUG
#include <iostream>
#include "gurls++/gurls.h"
#include "gurls++/exceptions.h"
#include "gurls++/gmat2d.h"
#include "gurls++/options.h"
#include "gurls++/optlist.h"
#include "gurls++/norm.h"
#include "gurls++/normzscore.h"
#include "gurls++/normtestzscore.h"
#include "gurls++/norml2.h"
#include "gurls++/gmath.h"
#include <kdl_codyco/utils.hpp>
#include <kdl/rigidbodyinertia.hpp>
#include <kdl/frames_io.hpp>
#include <kdl_codyco/regressors/dataset/DynamicDatasetFile.hpp>
#include <kdl_codyco/regressors/dynamicRegressorGenerator.hpp>
#include <cstdlib>
#include <iCub/iDynTree/iCubTree.h>
#include <iCub/ctrl/filters.h>
#include <iCub/ctrl/adaptWinPolyEstimator.h>
#include <assert.h>
#include <kdl_codyco/regressor_loops.hpp>
#include <kdl_codyco/regressor_utils.hpp>


#include <fstream>

#include <yarp/os/Property.h>

#include "multitaskRecursiveLinearEstimator.h"
#include "multitaskSVDLinearEstimator.h"



using namespace KDL;
using namespace KDL::CoDyCo;
using namespace KDL::CoDyCo::Regressors;
using namespace gurls;
using namespace std;

#define MODEL_DOFS 32
#define DATASET_DOFS 41

#define LEFT_ELBOW_TORQUE_INDEX 2
#define RIGHT_ELBOW_TORQUE_INDEX 8

#define LEFT_SHOULDER_PITCH_TORQUE_INDEX 6

#define LEFT_ARM_FT_INDEX 0
#define RIGHT_ARM_FT_INDEX 1

#define LEFT_ELBOW_GAIN 3.5290

#define LEFT_SHOULDER_PITCH_GAIN 2.4191

/** Tolerance for considering two values equal */
const double ZERO_TOL = 1e-5;

/**
 * The set of joint that are found in the dataset (i.e. head+torso+right_arm+left_arm) is different from
 * the joints found in the model, so we hat to convert them
 */
bool convertJointsFromDatasetToModel(const KDL::JntArray & q_dataset, KDL::JntArray & q_model, bool verbose = false)
{
    if( q_dataset.rows() != DATASET_DOFS ) {
        if( verbose ) cout << "Q from dataset has size " << q_dataset.rows() << std::endl;
    }

    assert(q_dataset.rows() == DATASET_DOFS);

    assert(q_model.rows() == MODEL_DOFS);
    //Joints in dataset are : Torso (3), head(6), left arm (16), right_arm (16)
    //Joints in the model are : Torso (3), head(3), left arm (7), right arm (7), left_leg (6), right_leg(6)
    
    SetToZero(q_model);
    
    //Torso should be inverted
    q_model(0) = q_dataset(2);
    q_model(1) = q_dataset(1);
    q_model(2) = q_dataset(0);
    
    //Copyng head
    for(int i = 0; i < 3; i++ ) {
        q_model(3+i) = q_dataset(3+i);
    }
    
    //Copyng left arm
    for(int i = 0; i < 7; i++ ) {
        q_model(3+3+i) = q_dataset(3+6+i);
    }
    
    //Copyng right arm 
    for(int i = 0; i < 7; i++ ) {
        q_model(3+3+7+i) = q_dataset(3+6+16+i);
    }
    
    return true;
//     cout <<"q_model data used for gurls for computing regularization parameter"<<std::endl;
//     cout << "=================================================================" << std::endl;
//     cout <<q_model <<std::endl;
//     cout << "================================================================="<<std::endl;
}

bool toYarp(const KDL::Wrench & ft, yarp::sig::Vector & ft_yrp)
{
    if( ft_yrp.size() != 6 ) { ft_yrp.resize(6); }
    for(int i=0; i < 6; i++ ) {
        ft_yrp[i] = ft[i];
    }
}

bool toKDL(const yarp::sig::Vector & ft_yrp, KDL::Wrench & ft)
{
    for(int i=0; i < 6; i++ ) {
        ft[i] = ft_yrp[i];
    }
}

bool toYarp(const Eigen::VectorXd & vec_eigen, yarp::sig::Vector & vec_yrp)
{
    if( vec_yrp.size() != vec_eigen.size() ) { vec_yrp.resize(vec_eigen.size()); }
    if( memcpy(vec_yrp.data(),vec_eigen.data(),sizeof(double)*vec_eigen.size()) != NULL ) {
        return true;
    } else {
        return false;
    }
}

bool toEigen(const yarp::sig::Vector & vec_yrp, Eigen::VectorXd & vec_eigen)
{
 if( vec_yrp.size() != vec_eigen.size() ) { vec_eigen.resize(vec_yrp.size()); }
    if( memcpy(vec_eigen.data(),vec_yrp.data(),sizeof(double)*vec_eigen.size()) != NULL ) {
        return true;
    } else {
        return false;
    }
}

typedef double T;

int main(int argc, char ** argv)
{
    yarp::os::Property opt;
    
    opt.fromCommand(argc,argv);
   
    if( !opt.check("dataset") ) {
        std::cout << "iCubParis02_data_analysis_simple: " << std::endl;
        cout << "usage: ./iCubParis02_data_analysis --dataset dataset.csv --results results.csv --xtrain xtr.txt --ytrain ytr.txt " << std::endl;
        cout << "additional options: --n_samples n the number of random regressors to generate for the numerical identifable subspace computation" << std::endl;
        cout << "additional options: --cut_freq the cut frequency to use for filtering torques and ft measures (in Hz, default 3 Hz)" << std::endl;
        cout << "additional options: --sampleTime the sample time of the data (in s, default: 0.01 s)" << std::endl;
        cout << "additional options: --lambda regularization parameter" << std::endl;
        cout << "additional options: --skip number of initial samples to skip" << std::endl;
        cout << "additional options: --verbose print debug information" << std::endl;
        cout << "additional options: --parameters param.csv output a file of trajectoreis of the estimated parameters" << std::endl;
        return 0;
    }
    
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    //// Parameter parsing
    //////////////////////////////////////////////////////////////////////////////////////////////////////////////
    
    // get file names from input
    string xtr_file;
    if(!opt.check("xtrain")) {
        cout << "Training samples file not specified. Use --xtrain [FILE_PATH]" << endl;
        return -1;
    } else {
        xtr_file = opt.find("xtrain").asString();
    }

    string ytr_file;
    if(!opt.check("ytrain")) {
        cout << "Training labels file not specified. Use --ytrain [FILE_PATH]" << endl;
        return -1;
    } else {
        ytr_file = opt.find("ytrain").asString();
    }
    
    int n_samples;
    if(!opt.check("n_samples")) {
        n_samples = 1000;
    } else {
        n_samples = opt.find("n_samples").asInt();
    }
    
    double sampleTime;
    if(!opt.check("sampleTime")) {
        sampleTime = 0.01;
    } else {
        sampleTime = opt.find("sampleTime").asDouble();
    }
    
        
    double cut_freq;
    if(!opt.check("cut_freq")) {
        cut_freq = 3;
    } else {
        cut_freq = opt.find("cut_freq").asDouble();
    }
    // for computing lambda using GURLS
    double lambda, lambda_semi;
    if(!opt.check("lambda")) 
    {
        lambda = 1.0; // Default lambda value
        
    }
//         typedef double T;
//         gMat2D<T> Xtr, ytr;	
// 	
// 	// for silvio dataset
// //      Xtr.readCSV("../build/Data_backup/Final_I_can_say/Silvio data/inputs.csv");
// //      ytr.readCSV("../build/Data_backup/Final_I_can_say/Silvio data/outputs.csv");
// 	
// 	// for icub dataset
// 	//Xtr.readCSV("../Data_Sets/Xtr_2psmall.txt");
// 	//cout << Xtr.rows() <<std::endl;
// 	//ytr.readCSV("../Data_Sets/ytr_2psmall.txt");
// 
//         // for given dataset
//         Xtr.readCSV(xtr_file);
//         cout << Xtr.rows() << endl;
//         ytr.readCSV(ytr_file);
//         
// 	OptTaskSequence *seq = new OptTaskSequence();
// 	*seq << "split:ho"<<"paramsel:hoprimal"; // paramsel can be used are fixlambda (always 1), loocvprimal, hoprimal
//         
//         
// 	cout << "========================================" << std::endl;
// 	cout << "Specified Task Sequence :" << std::endl;
// 	cout << *seq << std::endl <<std::endl; 
//         
//         
// 	GurlsOptionsList * process = new GurlsOptionsList("processes", false);
// 	OptProcess* process1 = new OptProcess();
// 	*process1 << GURLS::computeNsave << GURLS::computeNsave;
// 	process->addOpt("one", process1);
// 	GurlsOptionsList* apt = new GurlsOptionsList("iCubParis02_simple_analysis", true);
// 	apt->addOpt("seq", seq);
// 	apt->addOpt("processes", process);
//         OptString* hofun = new OptString("rmse");
//         apt->removeOpt("hoperf");
//         apt->addOpt("hoperf", hofun);
// 
// 	 GURLS G;
// 
//         string jobId0("one");
// 
//         // run gurls for getting regularization parameter (lambda)
//         G.run(Xtr, ytr, *apt, jobId0);
//         
// 	try{
//             gMat2D<T> &lambdas = apt->getOptValue<OptMatrix<gMat2D<T> > >("paramsel.lambdas");
//             
//             cout << "lambda values: \n" << lambdas << std::endl << std::endl;
//             
//             if (lambdas.getSize() > 1)
//             {
//                 gVec<T> d = lambdas.asvector();
//                 double a[6] = {d[0],d[1],d[2],d[3],d[4],d[5]};
//                 
//                 // Compute median lambda
//                 Median fun;
//                 lambda_semi = fun(a, 6); 
//                 
//                 if(lambda_semi != 1)
//                 {
//                     lambda = lambda_semi;
//                     cout <<"======================" <<endl;
//                     cout <<"Selected lambda: " << lambda <<endl;
//                     cout <<"======================" <<endl;
//                 }
//             }
//             
// //             else 
// //             { 
// //                 lambda = 1;
// //                 cout <<"final lambda value: " <<lambda<<endl;
// //             }
//        }
//        
//        catch(std::exception &){
//            cout << "Lambda not computed by GURLS" << std::endl << std::endl;
//        }
//        
//        //Debugging mode to see whether lambda value is correct or not....! optional
//        cout << " ================= Printing All ================" <<std::endl << std::endl;
//        apt->printAll();
//        cout << " ================= Printed All ================" <<std::endl << std::endl;
//     }
    
    else 
    {
        // Get lambda from parsed options
        lambda = opt.find("lambda").asDouble();
    }
    
    cout << "Selected lambda: " << lambda << endl;
    
    int skip;
    if(!opt.check("skip")) 
    {
        skip = 0;
    } 
    else 
    {
//         skip = opt.find("skip").asDouble();
        skip = opt.find("skip").asInt();
    }
    
    bool verbose;
    if(!opt.check("verbose")) 
    {
        verbose = false;
    } 
    else 
    {
        verbose = true;
    }
    
    //////////////////////////////////////////////////////////////////////////////////
    /// Opening the dataset in csv format. 
    /// The format of the dataset is described in https://github.com/traversaro/dir-dataset-format/blob/master/dynamic_dataset_format.md 
    //////////////////////////////////////////////////////////////////////////////////

    
    std::string dataset_file_name(opt.find("dataset").asString());
    //Loading dataset
    DynamicDatasetFile test_dataset;
    test_dataset.loadFromFile(dataset_file_name);
    
    
    ///////////////////////////////////////////////////////////////////////////////////
    //// Open a result file to dump output
    ///////////////////////////////////////////////////////////////////////////////////
    
    
    std::string results_file_name(opt.find("results").asString());
    //Open result file
    std::ofstream results_file;
    results_file.open(results_file_name.c_str());
    
    
    //////////////////////////////////////////////////////////////
    ////////// Get file name for estimated parameters dumping
    //////////////////////////////////////////////////////////
    
    
    bool output_param = false;
    std::string params_file_name;
    
    if(  opt.check("parameters") ) {
        params_file_name = opt.find("parameters").asString().c_str();
        output_param = true;
    }
    
    std::ofstream param_file;
    
    if( output_param ) {
        param_file.open(params_file_name.c_str());
    }
    
    
    /////////////////////////////////////////////////////////////////////////////////////
    /////// We get the model of the robot 
    //////////////////////////////////////////////////////////////////////////////////
    
    iCub::iDynTree::iCubTree_version_tag tag;
    //iCubParis02 is a v2 robot
    tag.head_version = 2;
    tag.legs_version = 2;
    
    iCub::iDynTree::iCubTree icub_tree_model(tag);
    
    
    //////////////////////////////////////////////////////////////////
    /////// We define the subtree we are interested into
    /////////////////////////////////////////////////////////////
    
    std::vector<std::string> l_arm_subtree, r_arm_subtree;
    l_arm_subtree.push_back("l_arm");
    r_arm_subtree.push_back("r_arm");
    
    
    ////////////////////////////////////////////////////////////////
    ///// All variables set to right part 
    ///////////////////////////////////////////////////////////////
    
    //Can be l_elbow or r_elbow
    std::string elbow_joint_name = "l_elbow";
    
    std::string shoulder_pitch_joint_name = "l_shoulder_pitch";
    
 
    
    //Can be RIGHT_ELBOW_TORQUE_INDEX or LEFT_ELBOW_TORQUE_INDEX
    const int elbow_torque_global_id = LEFT_ELBOW_TORQUE_INDEX;
    
    const int shoulder_pitch_global_id = LEFT_SHOULDER_PITCH_TORQUE_INDEX;
    
    //Can be r_arm_subtreee or l_arm_subtree
    std::vector<std::string> arm_subtree = l_arm_subtree;
    
    //In the regressor generator, the ft is identified by an index
    //We add only a FT sensor for regressor, so it should be 0
    const int ft_regressor_generator_sensor_index = 0;
    
    //Can be RIGHT_ARM_FT_INDEX or LEFT_ARM_FT_INDEX
    const int ft_dataset_sensor_index = LEFT_ARM_FT_INDEX;
    
    //Can be l_upper_arm or r_upper_arm
    std::string subtree_dynamic_base = "l_upper_arm";
    
    //Can be right or left (gain between raw outputs)
    const double elbow_torque_gain = LEFT_ELBOW_GAIN; 
    
    const double shoulder_pitch_torque_gain = LEFT_SHOULDER_PITCH_GAIN;
    
    
    ///////////////////////////////////////////////////////////////
    /////// Defining the names of the ft sensors
    ////////////////////////////////////////////////////////////////
    
    //Can be l_arm_ft_sensor or r_arm_ft_sensor 
    std::string ft_sensor_name = "l_arm_ft_sensor";
    std::vector<std::string> ft_names;
    ft_names.push_back(ft_sensor_name);
    
    
    ///////////////////////////////////////////////////////////////
    ///// Some links that are always considered to have 0 mass and not considered in parametric estimation
    ///////////////////////////////////////////////////////////
    
    std::vector<std::string> fake_names;
    fake_names.push_back("torso");
    fake_names.push_back("imu_frame");
    fake_names.push_back("l_gripper");
    fake_names.push_back("r_gripper");    
    fake_names.push_back("l_sole");
    fake_names.push_back("r_sole");
    fake_names.push_back("l_wrist_1");
    fake_names.push_back("r_wrist_1");
    
    
    //////////////////////////////////////////////
    ////// Root link of the overall robot
    ///////////////////////////////////////////////
    
    std::string root_link_name = "root_link";
   
    
    //////////////////////////////////////////////////
    /// Some data structures used to generate
    /////////////////////////////////////////////////
    
    /**
    * Get a list of wrenches that are the internal dynamics (base link M(q)ddq + C(q,dq)dq + n(q))
    * of each subtree, expressed in the world reference frame, with respect to the world origin
    */
    
    KDL::Tree icub_kdl_tree = icub_tree_model.getKDLTree();
    cout << "icub_kdl_tree information :" << icub_kdl_tree.getNrOfSegments() <<std::endl;
    
    KDL::CoDyCo::UndirectedTree icub_kdl_undirected_tree = icub_tree_model.getKDLUndirectedTree();
//     cout << "icub_kdl_undirected_tree information :" << icub_tree_model.getKDLUndirectedTree().getSerialization() <<std::endl;
    
    KDL::CoDyCo::TreeSerialization icub_serialization = icub_kdl_undirected_tree.getSerialization();
    cout << "icub_serialization information :" << icub_serialization.getNrOfLinks() << std::endl;
    
    KDL::CoDyCo::FTSensorList icub_ft_list(icub_kdl_undirected_tree,ft_names);
    cout <<"icub_ft_list information :" << icub_ft_list.getNrOfFTSensors() << std::endl;
 
    
    bool consider_ft_offset = true;
    bool verbose_output = true;
    
    /**
    * The dynamics regressor generator is a class for calculating arbitrary regressor
    * related to robot dynamics, for identification of dynamics parameters, such 
    * as inertial parameters (masses, centers of mass, inertia tensor elements) or 
    * other related parameters (for example force/torque sensor offset).
    */
    
    DynamicRegressorGenerator ft_regressor_generator(icub_kdl_tree,
                                                     root_link_name,
                                                     ft_names,
                                                     consider_ft_offset,
                                                     fake_names,
                                                     icub_serialization,
                                                     verbose_output);
    
    //For changing the base link to left or right arm
    ft_regressor_generator.changeDynamicBase(subtree_dynamic_base);


    //Adding one regressor to the generator:
    //    (1) 6 for ft sensors (with offset)
    int ret = ft_regressor_generator.addSubtreeRegressorRows(arm_subtree);
    assert(ret == 0);
        
    
    //Preprocessing of input
    iCub::ctrl::FirstOrderLowPassFilter * tau_filt;    
    iCub::ctrl::FirstOrderLowPassFilter * tau_filt_shoulder_pitch;
    iCub::ctrl::FirstOrderLowPassFilter * ft_filt;
    iCub::ctrl::AWLinEstimator * velocity_estimator;
    iCub::ctrl::AWPolyEstimator * acceleration_estimator;
    
   
    //Defining some variables used in the estimation loop
    KDL::JntArray q, dq, ddq;
    yarp::sig::Vector q_yarp, dq_yarp, ddq_yarp;
    
    q.resize(ft_regressor_generator.getNrOfDOFs());
    SetToZero(q);
    
    q_yarp.resize(q.rows(),0.0);
    dq_yarp = q_yarp;
    ddq_yarp = q_yarp;
    
    cout << ddq_yarp.size() << std::endl;
    
    dq = q;
    ddq = q;
    
    cout << "rows :" << ddq.rows()<<" cols:" << ddq.columns() <<std::endl;
    KDL::Wrench ft_sample;
    cout << ft_sample << std::endl;
    yarp::sig::Vector ft_sample_raw(6,0.0), ft_sample_filtered(6,0.0);
    
    //Defining gravity vector
    const double g = 9.806;
    KDL::Twist gravity(KDL::Vector(0.0,0.0,g) , KDL::Vector(0.0,0.0,0.0));
    
//     KDL::Twist zero_gravity = KDL::Twist::Zero();
//     KDL::JntArray zero_q = q;
//     SetToZero(zero_q);
    
    cout << "ft_regressor_generator.getNrOfParameters() = " << ft_regressor_generator.getNrOfParameters() << std::endl;
    cout << "ft_regressor_generator.getNrOfOutputs() = " << ft_regressor_generator.getNrOfOutputs() << std::endl;
    
    //Defining the matrix that i will used for getting the regressor
    Eigen::MatrixXd ft_regressor(ft_regressor_generator.getNrOfOutputs(),ft_regressor_generator.getNrOfParameters());
    cout <<"rows of ft_regressor :" <<ft_regressor.rows()<<"cols of ft_regressor :" << ft_regressor.cols()<<std::endl;
    Eigen::VectorXd ft_kt(ft_regressor_generator.getNrOfOutputs());
    cout <<"rows of ft_kt :" <<ft_kt.rows()<<"cols of ft_regressor :" << ft_kt.cols()<<std::endl;
    Eigen::MatrixXd ft_regressor_wrong_frame(ft_regressor_generator.getNrOfOutputs(),ft_regressor_generator.getNrOfParameters());
    cout <<"rows of ft_regressor_wrong_frame :" <<ft_regressor_wrong_frame.rows()<<"cols of ft_regressor_wrong_frame :" << ft_regressor_wrong_frame.cols()<<std::endl;
    Eigen::VectorXd ft_kt_wrong_frame(ft_regressor_generator.getNrOfOutputs());
    cout <<"rows of ft_kt_wrong_frame :" <<ft_kt_wrong_frame.rows()<<"cols of ft_regressor :" << ft_kt_wrong_frame.cols()<<std::endl;
    
    
  
    int ret_value = 0;
    
    //Computing the identifiable subspace basis matrix
    Eigen::MatrixXd base_parameters_subspace; 
    cout << "rows of base_parameters_subspace :" <<base_parameters_subspace.rows() << endl <<"cols of base_parameters_subspace :" <<base_parameters_subspace.cols() <<std::endl;
    cout << "n_samples: " << n_samples<<std::endl;
    
    //ret_value = ft_regressor_generator.computeNumericalIdentifiableSubspace(base_parameters_subspace,false,n_samples);    // WARNING: SEEMS WRONG, outputs NaNs
    ret_value = ft_regressor_generator.computeNumericalIdentifiableSubspace(base_parameters_subspace);
    
    cout << "ret_value: " << ret_value << std::endl;
    cout << "base_parameters_subspace rows: " << base_parameters_subspace.rows() << endl;
    cout << "base_parameters_subspace cols: " << base_parameters_subspace.cols() << endl;
    assert( ret_value == 0 );
    
    std::cout << "Computed base_parameters_subspace:" <<std::endl << base_parameters_subspace << std::endl;
    
    
    //Computing the identifiable subspace basis matrix, considering velocity and acceleration to zero
    //Eigen::MatrixXd base_parameters_subspace; 
    //ret_value = ft_regressor_generator.computeNumericalIdentifiableSubspace(base_parameters_subspace,false,n_samples);
    //assert( ret_value == 0 );
    
    //int nrOfStatic_base_parameters = static_base_parameters_subspace.cols();
    int nrOfBase_parameters = base_parameters_subspace.cols();
    
    cout << "nrOfBase_parameters = " << nrOfBase_parameters << std::endl;
    
    //Defining the hyperparameter of output standard deviation
    Eigen::VectorXd output_stddev(6);
    cout << "WARNING: Output standard deviatio hyperparameter is set manually" << endl;
    output_stddev << 0.0707119 ,  0.07460326,  0.11061799,  0.00253377,  0.00295331, 0.00281101;
   
   
    //Defining the estimator objects
    multiTaskRecursiveLinearEstimator estimator_dynamic(nrOfBase_parameters,ft_regressor_generator.getNrOfOutputs(),lambda);
    estimator_dynamic.setOutputErrorStandardDeviation(output_stddev);
    
    //multiTaskRecursiveLinearEstimator estimator_static(static_base_parameters,ft_regressor_generator.getNrOfOutputs(),lambda);
    //estimator_static.setOutputErrorStandardDeviation(output_stddev);
    
       
    // Function to output to file estimated parameters
//     if( output_param ) {
//         //Print outputs
//         params_file_name;
//     }
    
    // Get cad parameters from model
    Eigen::VectorXd cad_parameters(ft_regressor_generator.getNrOfParameters());
    cad_parameters.setZero();
    KDL::CoDyCo::inertialParametersVectorLoopFakeLinks(icub_kdl_undirected_tree,cad_parameters,fake_names);
    
    //For comparing estimates between cad and estimated parameters, we have to estimate
    // the offset for the cad
    
    //Structures for offset estimation
    
    multiTaskSVDLinearEstimator estimator_static_offset(6,6);
    int sample_nr;
    int nmbr_of_samples_for_offset_calibration = 30;
    int nmbr_of_samples_for_offset_calibration_obtained = 0;
    Eigen::VectorXd offset = Eigen::VectorXd(6);
    Eigen::MatrixXd regressor_offset;
    Eigen::VectorXd offset_kt;
    cout << "Number of samples" << sample_nr << endl;
    
    cout << "size of test_dataset rows:" << test_dataset.getNrOfSamples() <<endl;
    
    //Estimation loop
    for(sample_nr = skip; sample_nr < test_dataset.getNrOfSamples(); ++sample_nr ) 
    {
        if( verbose && sample_nr % 1000 == 0 )
        {
            cout << "Looping on sample: " << sample_nr << endl;
        }
        
        //Joints in dataset are : Torso (3), head(6), left arm (16), right_arm (16)
        //Depending on the dataset we use different parts, here we use only the arm, the torso and the head (which however should be still)
        DynamicSample sample;
        bool ret = test_dataset.getSample(sample_nr,sample);
        if( !ret ) return -1;
        assert(sample.getNrOfDOFs() == DATASET_DOFS);
        assert( sample.getJointPosition().rows() == DATASET_DOFS);
       
        convertJointsFromDatasetToModel(sample.getJointPosition(),q);
	//cout << q.rows() << q.columns() <<q.data <<std::endl;
        
        //Filter values
	//cout << "sample.getNrOfDOFs() = " << sample.getNrOfDOFs()<<std::endl;

	//cout << sample.getJointVelocity() <<sample.getJointVelocity() << sample.getJointAcceleration() <<sample.getNrOfTorqueSensors() <<sample.getTorqueMeasure()<<std::endl;
        ft_sample = sample.getWrenchMeasure(ft_dataset_sensor_index);
        
        if( verbose && sample_nr % 1000 == 0 ) {
            cout << "FT sample from dataset" << std::endl;
            cout << ft_sample << std::endl;
            cout << "Force norm: "<<  ft_sample.force.Norm() << std::endl;
        }
        
        //cout<< "Sample: " << ft_sample.force << "   " << ft_sample.torque << std::endl;
        //cout <<"size: " << ft_sample_raw.size() <<" data: " << ft_sample_raw.data() << " length: " << ft_sample_raw.length()<< std::endl;
        
        toYarp(ft_sample,ft_sample_raw);
        
        //If the sample is the first one (skip) allocate the memory, with all the parameters
        if( sample_nr == skip ) 
        {
            ft_filt = new iCub::ctrl::FirstOrderLowPassFilter(cut_freq,sampleTime,ft_sample_raw);
            velocity_estimator = new iCub::ctrl::AWLinEstimator(16,0.02);
            
	    //cout << velocity_estimator->getList() << std::endl;
            
            acceleration_estimator = new iCub::ctrl::AWQuadEstimator(50,0.02);
	    
            //cout << acceleration_estimator->getList() << std::endl;
        } 
        else 
        {
            ft_sample_filtered = ft_filt->filt(ft_sample_raw);
            toKDL(ft_sample_filtered,ft_sample);
        }
        
        if( verbose && sample_nr % 1000 == 0 ) {
            cout << "FT sample filtered" << std::endl;
            cout << ft_sample << std::endl;
            cout << "Force norm: "<<  ft_sample.force.Norm() << std::endl;
        }
        
        //Obtain velocities and accelerations 
        iCub::ctrl::AWPolyElement el;
        toYarp(q.data,el.data);
        el.time = sample.getTimestamp();
        toEigen(velocity_estimator->estimate(el),dq.data);
        toEigen(acceleration_estimator->estimate(el),ddq.data);
	
        //cout << "velocity data : " << std::endl << dq.data << std::endl;
	//cout << "accerlation data : " << std::endl << ddq.data << std::endl;
	//cout << estimator_dynamic.getParamSize() << estimator_dynamic.getParameterEstimate().data() << estimator_dynamic.getParameterEstimate() <<std::endl;        
        
        //Compute the regressors !
        ft_regressor_generator.setRobotState(q,dq,ddq,gravity);
        ft_regressor_generator.setFTSensorMeasurement(ft_regressor_generator_sensor_index,ft_sample);
        //ft_regressor_generator.setTorqueSensorMeasurement(icub_tree_model.getDOFIndex(elbow_joint_name),torque_sample);
        ft_regressor_generator.computeRegressor(ft_regressor_wrong_frame,ft_kt_wrong_frame);

        //Change frame of reference of ft_regressor and ft_kt
        //They should be on the r_upper_arm or l_upper_arm (parent of the sensor)
        //and we want it in the sensor reference frame
        KDL::Frame H_sensor_parent = icub_ft_list.ft_sensors_vector[0].getH_parent_sensor().Inverse();
        Eigen::Matrix<double,6,6> H_sens_parent = KDL::CoDyCo::WrenchTransformationMatrix(H_sensor_parent); 
        ft_regressor = H_sens_parent * ft_regressor_wrong_frame;
        ft_kt = H_sens_parent * ft_kt_wrong_frame;
        
        if( verbose && sample_nr % 1000 == 0) {
            cout << "Known term for forcetorque based regression (wrong frame)" << std::endl;
            cout << ft_kt_wrong_frame << std::endl;;
            cout << "Known term for forcetorque based regression" << std::endl;
            cout << ft_kt << std::endl;
            cout << H_sensor_parent << std::endl;
            cout << H_sens_parent << std::endl;
            cout << "Force norm: " << ft_kt.head(3).norm() << std::endl;
        }
        
        //Compute offset for CAD parameters
        //WARNING: This should be a while loop
        if( nmbr_of_samples_for_offset_calibration_obtained < nmbr_of_samples_for_offset_calibration ) {
            regressor_offset = ft_regressor.rightCols<6>(); 
            offset_kt = ft_kt - ft_regressor * cad_parameters;
            
            estimator_static_offset.feedSample(regressor_offset,offset_kt);
            nmbr_of_samples_for_offset_calibration_obtained++;
            
            if( nmbr_of_samples_for_offset_calibration_obtained == nmbr_of_samples_for_offset_calibration ) {
                estimator_static_offset.updateParameterEstimate();
                offset = estimator_static_offset.getParameterEstimate();
                assert(offset.size() == 6);
                cad_parameters.tail<6>() = offset;
            }
        }
        
        //Predicting output given the current estimate
        Eigen::Matrix<double,6,1> ft_estimated_prediction =  ft_regressor * base_parameters_subspace * estimator_dynamic.getParameterEstimate();
        Eigen::Matrix<double,6,1> ft_cad_prediction =  ft_regressor * cad_parameters;
        //cout << ft_estimated_prediction.data() << std::endl;
//         cout << "ft_regressor:" <<endl<< ft_regressor << std::endl;
//         cout << "base_parameters_subspace:" <<endl<< base_parameters_subspace << std::endl;
//         cout << "estimator_dynamic.getParameterEstimate():" <<endl<< estimator_dynamic.getParameterEstimate() << std::endl;
//         cout << "ft_estimated_prediction:" <<endl<< ft_estimated_prediction << std::endl;
	//cout << ft_cad_prediction.data() << std::endl;

        //Updating the estimate
        estimator_dynamic.feedSampleAndUpdate(ft_regressor * base_parameters_subspace,ft_kt);
           
        //estimator_static.feedSampleAndUpdate(ft_regressor*static_base_parameters_subspace,ft_kt);
                
        //Adding the data to the results file
        for(int i=0; i < 6; i++ ) 
        {
            //cout << ft_estimated_prediction[i] << std::endl;
            results_file << ft_estimated_prediction[i] << ",";
        }
        
        for(int i=0; i < 6; i++ ) 
        {
            results_file << ft_cad_prediction[i]; 
	    //cout << ft_cad_prediction[i] << std::endl;
            if( i != 6 ) 
            {
                results_file << ",";
            } 
//             else 
//             {
//                 results_file << std::endl;
//             }
        }
        results_file << std::endl;
      
        if( output_param ) {
            Eigen::VectorXd params1 = estimator_dynamic.getParameterEstimate();
            for(int param_nr=0; param_nr < params1.size(); param_nr++ ) 
            {
                if( param_nr < params1.size() ) 
                {
		    //cout << param_nr <<params1[param_nr] << std::endl;
                    param_file << params1[param_nr];
                } 
                if( param_nr != params1.size()-1 ) 
                {
                    param_file << ",";
                } 
//                 else 
//                 {
//                     param_file << std::endl;
//                 }
            }
            param_file << std::endl;
        }
                     
        if( sample_nr % 1000 == 0 ) { cout << "Processing sample " << sample_nr << " with real_torque " << elbow_torque_gain*sample.getTorqueMeasure(elbow_torque_global_id) <<  std::endl;
                                      cout << "Estimated static parameters:" <<endl<< estimator_dynamic.getParameterEstimate() << std::endl;
                                      cout << "ft_estimated_prediction:" <<endl<< ft_estimated_prediction << std::endl;
        }
    }
   
    results_file.close();
    
    if( output_param ) {
        param_file.close();
    }
    
    cout << "iCubParis02_analysis_simple: saved result to " << results_file_name << std::endl;
    if( output_param ) {
        cout << "iCubParis02_analysis_simple: saved parameters to " << params_file_name << std::endl;
    }
    cout << "iCubParis02_analysis_simple: got " << sample_nr << " samples."  << std::endl;
    
    return 0;
}
