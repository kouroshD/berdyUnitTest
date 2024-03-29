/*
 * Copyright (C) 2016 Fondazione Istituto Italiano di Tecnologia
 *
 * Licensed under either the GNU Lesser General Public License v3.0 :
 * https://www.gnu.org/licenses/lgpl-3.0.html
 * or the GNU Lesser General Public License v2.1 :
 * https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html
 * at your option.
 */

#include <iDynTree/Estimation/BerdyHelper.h>
#include <iDynTree/Estimation/ExtWrenchesAndJointTorquesEstimator.h>

#include <iDynTree/Sensors/PredictSensorsMeasurements.h>

#include "testModels.h"
#include <iDynTree/Core/EigenHelpers.h>
#include <iDynTree/Core/EigenSparseHelpers.h>
#include <iDynTree/Core/TestUtils.h>
#include <iDynTree/Core/SparseMatrix.h>
#include <ModelTestUtils.h>

#include <iDynTree/Model/ForwardKinematics.h>
#include <iDynTree/Model/Dynamics.h>
#include <iDynTree/Estimation/BerdySparseMAPSolver.h>


#include <cstdio>
#include <cstdlib>

using namespace iDynTree;


struct BerdyData
{
    std::unique_ptr<iDynTree::BerdySparseMAPSolver> solver = nullptr;
    iDynTree::BerdyHelper helper;

    struct Priors
    {
        // Regularization priors
        iDynTree::VectorDynSize dynamicsRegularizationExpectedValueVector; // mu_d
        iDynTree::SparseMatrix<iDynTree::ColumnMajor> dynamicsRegularizationCovarianceMatrix; // sigma_d

        // Dynamic constraint prior
        iDynTree::SparseMatrix<iDynTree::ColumnMajor> dynamicsConstraintsCovarianceMatrix; // sigma_D

        // Measurements prior
        iDynTree::SparseMatrix<iDynTree::ColumnMajor> measurementsCovarianceMatrix; // sigma_y

        static void
        initializeSparseMatrixSize(size_t size,
                                   iDynTree::SparseMatrix<iDynTree::ColumnMajor>& matrix)
        {
            iDynTree::Triplets identityTriplets;
            identityTriplets.reserve(size);

            // Set triplets to Identity
            identityTriplets.setDiagonalMatrix(0, 0, 1.0, size);

            matrix.resize(size, size);
            matrix.setFromTriplets(identityTriplets);
        }
    } priors;

    struct Buffers
    {
        iDynTree::VectorDynSize measurements;

    } buffers;

    struct KinematicState
    {
        iDynTree::FrameIndex floatingBaseFrameIndex;

        iDynTree::Vector3 baseAngularVelocity;
        iDynTree::JointPosDoubleArray jointsPosition;
        iDynTree::JointDOFsDoubleArray jointsVelocity;
        iDynTree::JointDOFsDoubleArray jointsAcceleration;
    } state;

    struct DynamicEstimates
    {
        iDynTree::JointDOFsDoubleArray jointTorqueEstimates;
    } estimates;
};


void testBerdySensorMatrices(BerdyHelper & berdy, std::string filename)
{
    // Check the concistency of the sensor matrices
    // Generate a random pos, vel, acc, external wrenches
    FreeFloatingPos pos(berdy.model());
    FreeFloatingVel vel(berdy.model());
    FreeFloatingAcc generalizedProperAccs(berdy.model());
    LinkNetExternalWrenches extWrenches(berdy.model());

    getRandomInverseDynamicsInputs(pos,vel,generalizedProperAccs,extWrenches);

    // Force the base linear velocity to be zero for ensure consistency with the compute buffers
    vel.baseVel().setLinearVec3(LinVelocity(0.0, 0.0, 0.0));

    LinkPositions linkPos(berdy.model());
    LinkVelArray  linkVels(berdy.model());
    LinkAccArray  linkProperAccs(berdy.model());

    LinkInternalWrenches intWrenches(berdy.model());
    FreeFloatingGeneralizedTorques genTrqs(berdy.model());

    // Compute consistent joint torques and internal forces using inverse dynamics
    ForwardPosVelAccKinematics(berdy.model(),berdy.dynamicTraversal(),
                               pos, vel, generalizedProperAccs,
                               linkPos,linkVels,linkProperAccs);
    RNEADynamicPhase(berdy.model(),berdy.dynamicTraversal(),
                     pos.jointPos(),linkVels,linkProperAccs,
                     extWrenches,intWrenches,genTrqs);

    // Propagate kinematics also inside berdy

    // Correct for the unconsistency between the input net wrenches and the residual of the RNEA
    extWrenches(berdy.dynamicTraversal().getBaseLink()->getIndex()) = extWrenches(berdy.dynamicTraversal().getBaseLink()->getIndex())+genTrqs.baseWrench();

    // Generate the d vector of dynamical variables
    VectorDynSize d(berdy.getNrOfDynamicVariables());

    // LinkNewInternalWrenches (necessary for the old-style berdy)
    LinkNetTotalWrenchesWithoutGravity linkNetWrenchesWithoutGravity(berdy.model());

    for(LinkIndex visitedLinkIndex = 0; visitedLinkIndex < berdy.model().getNrOfLinks(); visitedLinkIndex++)
     {
         LinkConstPtr visitedLink = berdy.model().getLink(visitedLinkIndex);

         const iDynTree::SpatialInertia & I = visitedLink->getInertia();
         const iDynTree::SpatialAcc     & properAcc = linkProperAccs(visitedLinkIndex);
         const iDynTree::Twist          & v = linkVels(visitedLinkIndex);
         linkNetWrenchesWithoutGravity(visitedLinkIndex) = I*properAcc + v*(I*v);
     }

    // Get the angular v
    LinkIndex baseIdx = berdy.dynamicTraversal().getBaseLink()->getIndex();
    berdy.updateKinematicsFromFloatingBase(pos.jointPos(),vel.jointVel(),baseIdx,linkVels(baseIdx).getAngularVec3());

    berdy.serializeDynamicVariables(linkProperAccs,
                                    linkNetWrenchesWithoutGravity,
                                    extWrenches,
                                    intWrenches,
                                    genTrqs.jointTorques(),
                                    generalizedProperAccs.jointAcc(),
                                    d);


    // Check D and bD , in particular that D*d + bD = 0
    // Generated the Y e bY matrix and vector from berdy
    SparseMatrix<iDynTree::ColumnMajor> D, Y;
    VectorDynSize bD, bY;
    berdy.resizeAndZeroBerdyMatrices(D,bD,Y,bY);
    bool ok = berdy.getBerdyMatrices(D,bD,Y,bY);
    ASSERT_IS_TRUE(ok);

    VectorDynSize dynamicsResidual(berdy.getNrOfDynamicEquations()), zeroRes(berdy.getNrOfDynamicEquations());

    toEigen(dynamicsResidual) = toEigen(D)*toEigen(d) + toEigen(bD);

    /*
    std::cerr << "D : " << std::endl;
    std::cerr << D.description(true) << std::endl;
    std::cerr << "d :\n" << d.toString() << std::endl;
    std::cerr << "D*d :\n" << toEigen(D)*toEigen(d) << std::endl;
    std::cerr << "bD :\n" << bD.toString() << std::endl;
    */

    ASSERT_EQUAL_VECTOR(dynamicsResidual, zeroRes);
    
    if( berdy.getNrOfSensorsMeasurements() > 0 )
    {
        std::cout << "BerdyHelperUnitTest, testing sensors matrix for model " << filename <<  std::endl;

        // Generate the y vector of sensor measurements using the predictSensorMeasurements function
        VectorDynSize y(berdy.getNrOfSensorsMeasurements());
        y.zero();
        SensorsMeasurements sensMeas(berdy.sensors());
        bool ok = predictSensorsMeasurementsFromRawBuffers(berdy.model(),berdy.sensors(),berdy.dynamicTraversal(),
                                                           linkVels,linkProperAccs,intWrenches,sensMeas);

        ASSERT_IS_TRUE(ok);
        ok = berdy.serializeSensorVariables(sensMeas,extWrenches,genTrqs.jointTorques(),generalizedProperAccs.jointAcc(),intWrenches,y);
        ASSERT_IS_TRUE(ok);

        // Check that y = Y*d + bY
        VectorDynSize yFromBerdy(berdy.getNrOfSensorsMeasurements());

        ASSERT_EQUAL_DOUBLE(berdy.getNrOfSensorsMeasurements(), Y.rows());
        ASSERT_EQUAL_DOUBLE(berdy.getNrOfSensorsMeasurements(), bY.size());

        toEigen(yFromBerdy) = toEigen(Y)*toEigen(d) + toEigen(bY);

        //std::cerr << "Y : " << std::endl;
        //std::cerr << Y.description(true) << std::endl;
        //std::cerr << "d :\n" << d.toString() << std::endl;
        /*
        std::cerr << "Y*d :\n" << toEigen(Y)*toEigen(d) << std::endl;
        std::cerr << "bY :\n" << bY.toString() << std::endl;
        std::cerr << "y from model:\n" << y.toString() << std::endl;
        */

        // Check if the two vectors are equal
        ASSERT_EQUAL_VECTOR(y,yFromBerdy);
    }
}

/*
 * In the ORIGINAL_BERDY_FIXED_BASE, the serialization of the
 * dynamic variables returned by getDynamicVariablesOrdering
 * should be contiguous. Check this.
 */
void testBerdyOriginalFixedBaseDynamicEquationSerialization(BerdyHelper& berdy)
{
    std::vector<iDynTree::BerdyDynamicVariable> dynVarOrdering = berdy.getDynamicVariablesOrdering();

    // Variables containing the first index not described by dynVarOrdering
    size_t accumulator=0;
    for(size_t i=0; i < dynVarOrdering.size(); i++)
    {
        ASSERT_EQUAL_DOUBLE(accumulator,dynVarOrdering[i].range.offset);
        accumulator += dynVarOrdering[i].range.size;
    }

    // Once we finish, accumulator should be equal to the number of dyn equations
    ASSERT_EQUAL_DOUBLE(berdy.getNrOfDynamicVariables(),accumulator);
}

void testBerdyOriginalFixedBase(BerdyHelper & berdy, std::string filename)
{
    // Check the concistency of the sensor matrices
    // Generate a random pos, vel, acc, external wrenches
    FreeFloatingPos pos(berdy.model());
    FreeFloatingVel vel(berdy.model());
    FreeFloatingAcc generalizedProperAccs(berdy.model());
    LinkNetExternalWrenches extWrenches(berdy.model());

    getRandomInverseDynamicsInputs(pos,vel,generalizedProperAccs,extWrenches);

    Vector3 grav;
    grav.zero();
    grav(2) = -10;

    Vector3 baseProperAcc;
    baseProperAcc.zero();
    baseProperAcc(2) = -grav(2);

    // Set the base variables to zero for the fixed base case
    pos.worldBasePos() = Transform::Identity();
    vel.baseVel().zero();
    generalizedProperAccs.baseAcc().zero();
    generalizedProperAccs.baseAcc().setLinearVec3(baseProperAcc);

    LinkPositions linkPos(berdy.model());
    LinkVelArray  linkVels(berdy.model());
    LinkAccArray  linkProperAccs(berdy.model());

    LinkInternalWrenches intWrenches(berdy.model());
    FreeFloatingGeneralizedTorques genTrqs(berdy.model());

    // Compute consistent joint torques and internal forces using inverse dynamics
    ForwardPosVelAccKinematics(berdy.model(),berdy.dynamicTraversal(),
                               pos, vel, generalizedProperAccs,
                               linkPos,linkVels,linkProperAccs);
    RNEADynamicPhase(berdy.model(),berdy.dynamicTraversal(),
                     pos.jointPos(),linkVels,linkProperAccs,
                     extWrenches,intWrenches,genTrqs);

    // Correct for the unconsistency between the input net wrenches and the residual of the RNEA
    extWrenches(berdy.dynamicTraversal().getBaseLink()->getIndex()) = extWrenches(berdy.dynamicTraversal().getBaseLink()->getIndex())+genTrqs.baseWrench();

    // Generate the d vector of dynamical variables
    VectorDynSize d(berdy.getNrOfDynamicVariables());

    // LinkNewInternalWrenches (necessary for the old-style berdy)
    LinkNetTotalWrenchesWithoutGravity linkNetWrenchesWithoutGravity(berdy.model());

    for(LinkIndex visitedLinkIndex = 0; visitedLinkIndex < berdy.model().getNrOfLinks(); visitedLinkIndex++)
     {
         LinkConstPtr visitedLink = berdy.model().getLink(visitedLinkIndex);

         const iDynTree::SpatialInertia & I = visitedLink->getInertia();
         const iDynTree::SpatialAcc     & properAcc = linkProperAccs(visitedLinkIndex);
         const iDynTree::Twist          & v = linkVels(visitedLinkIndex);
         linkNetWrenchesWithoutGravity(visitedLinkIndex) = I*properAcc + v*(I*v);
     }

    // Get the angular v
    berdy.updateKinematicsFromFixedBase(pos.jointPos(),vel.jointVel(),berdy.dynamicTraversal().getBaseLink()->getIndex(),grav);

    berdy.serializeDynamicVariables(linkProperAccs,
                                    linkNetWrenchesWithoutGravity,
                                    extWrenches,
                                    intWrenches,
                                    genTrqs.jointTorques(),
                                    generalizedProperAccs.jointAcc(),
                                    d);

    // Check D and bD , in particular that D*d + bD = 0
    // Generated the Y e bY matrix and vector from berdy
    SparseMatrix<iDynTree::ColumnMajor> D, Y;
    VectorDynSize bD, bY;
    berdy.resizeAndZeroBerdyMatrices(D,bD,Y,bY);
    bool ok = berdy.getBerdyMatrices(D,bD,Y,bY);
    ASSERT_IS_TRUE(ok);

    VectorDynSize dynamicsResidual(berdy.getNrOfDynamicEquations()), zeroRes(berdy.getNrOfDynamicEquations());

    toEigen(dynamicsResidual) = toEigen(D)*toEigen(d) + toEigen(bD);

    /*
    std::cerr << "D : " << std::endl;
    std::cerr << D.description(true) << std::endl;
    std::cerr << "d :\n" << d.toString() << std::endl;
    std::cerr << "D*d :\n" << toEigen(D)*toEigen(d) << std::endl;
    std::cerr << "bD :\n" << bD.toString() << std::endl;
    */

    ASSERT_EQUAL_VECTOR(dynamicsResidual,zeroRes);


    if( berdy.getNrOfSensorsMeasurements() > 0 )
    {
        std::cout << "BerdyHelperUnitTest, testing sensors matrix for model " << filename <<  std::endl;

        // Generate the y vector of sensor measurements using the predictSensorMeasurements function
        VectorDynSize y(berdy.getNrOfSensorsMeasurements());
        y.zero();
        SensorsMeasurements sensMeas(berdy.sensors());
        ok = predictSensorsMeasurementsFromRawBuffers(berdy.model(),berdy.sensors(),berdy.dynamicTraversal(),
                                                      linkVels,linkProperAccs,intWrenches,sensMeas);
        ASSERT_IS_TRUE(ok);

        ok = berdy.serializeSensorVariables(sensMeas,extWrenches,genTrqs.jointTorques(),generalizedProperAccs.jointAcc(),intWrenches,y);
        ASSERT_IS_TRUE(ok);

        // Check that y = Y*d + bY
        VectorDynSize yFromBerdy(berdy.getNrOfSensorsMeasurements());

        toEigen(yFromBerdy) = toEigen(Y)*toEigen(d) + toEigen(bY);

        /*
        std::cerr << "Y : " << std::endl;
        std::cerr << Y.description(true) << std::endl;
        std::cerr << "d :\n" << d.toString() << std::endl;
        std::cerr << "Y*d :\n" << toEigen(Y)*toEigen(d) << std::endl;
        std::cerr << "bY :\n" << bY.toString() << std::endl;


        std::cerr << Y.description(true) << std::endl;
        std::cerr << "Testing " << berdy.getOptions().jointOnWhichTheInternalWrenchIsMeasured[0] << std::endl;
        std::cerr << intWrenches(berdy.model().getJointIndex(berdy.getOptions().jointOnWhichTheInternalWrenchIsMeasured[0])).toString() << std::endl;
        */

        // Check if the two vectors are equal
        ASSERT_EQUAL_VECTOR(y,yFromBerdy);
    }

    testBerdyOriginalFixedBaseDynamicEquationSerialization(berdy);
}

void testBerdyHelpers(std::string fileName)
{
    // \todo TODO simplify model loading (now we rely on teh ExtWrenchesAndJointTorquesEstimator
    ExtWrenchesAndJointTorquesEstimator estimator;
    bool ok = estimator.loadModelAndSensorsFromFile(fileName);

    ASSERT_IS_TRUE(estimator.sensors().isConsistent(estimator.model()));
    ASSERT_IS_TRUE(ok);
    class Impl;
    std::unique_ptr<Impl> pImpl;
//    pImpl{new Impl()};

    BerdyHelper berdyHelper;

    // First test the original BERDY
    BerdyOptions options;
    options.berdyVariant = iDynTree::ORIGINAL_BERDY_FIXED_BASE;
    options.includeAllJointAccelerationsAsSensors = false;
    options.includeAllNetExternalWrenchesAsSensors = false;

    // Add one arbitary joint wrench sensor
    if( estimator.model().getNrOfJoints() > 0 )
    {
        JointIndex jntIdx = estimator.model().getNrOfJoints()/2;
        options.jointOnWhichTheInternalWrenchIsMeasured.push_back(estimator.model().getJointName(jntIdx));
    }

    ok = false;
    //ok = berdyHelper.init(estimator.model(),estimator.sensors(),options);

    if( ok )
    {
        std::cerr << "Testing ORIGINAL_BERDY_FIXED_BASE tests for model " << fileName << " because the assumptions of ORIGINAL_BERDY_FIXED_BASE are respected" << std::endl;
        //testBerdyOriginalFixedBase(berdyHelper,fileName);
        // Change the options a bit and test again
        options.includeAllNetExternalWrenchesAsDynamicVariables = false;
        berdyHelper.init(estimator.model(),estimator.sensors(),options);
        //testBerdyOriginalFixedBase(berdyHelper,fileName);
    }
    else
    {
        std::cerr << "Skipping ORIGINAL_BERDY_FIXED_BASE tests for model " << fileName << " because some assumptions of ORIGINAL_BERDY_FIXED_BASE are not respected" << std::endl;
    }
    // HDE steps

    std::string baseLink = estimator.model().getLinkName(estimator.model().getDefaultBaseLink());
    std::cout<<"base link name: "<<baseLink<<std::endl;
    iDynTree::BerdyOptions berdyOptions;
    berdyOptions.baseLink = baseLink;
    berdyOptions.berdyVariant = iDynTree::BerdyVariants::BERDY_FLOATING_BASE;
    berdyOptions.includeAllNetExternalWrenchesAsSensors = true;
    berdyOptions.includeAllNetExternalWrenchesAsDynamicVariables = true;
    berdyOptions.includeAllJointAccelerationsAsSensors = true;
    berdyOptions.includeAllJointTorquesAsSensors = false;
    berdyOptions.includeFixedBaseExternalWrench = false;

    // Initialize the BerdyHelper
    if (!pImpl->berdyData.helper.init(modelLoader.model(), humanSensors, berdyOptions)) {
        yError() << LogPrefix << "Failed to initialize BERDY";
        return false;
    }

    // Check berdy options
    if (!berdyOptions.checkConsistency()) {
        std::cout<< "BERDY options are not consistent";
        return;
    }


    // We test the floating base BERDY
    options.berdyVariant = iDynTree::BERDY_FLOATING_BASE;
    // For now floating berdy needs all the ext wrenches as dynamic variables
    options.includeAllNetExternalWrenchesAsDynamicVariables = true;
    ok = berdyHelper.init(estimator.model(), estimator.sensors(), options);
    ASSERT_IS_TRUE(ok);
    testBerdySensorMatrices(berdyHelper, fileName);
    
    // Test includeAllJointTorqueAsSensors option 
    options.berdyVariant = iDynTree::BERDY_FLOATING_BASE;
    // For now floating berdy needs all the ext wrenches as dynamic variables
    options.includeAllNetExternalWrenchesAsDynamicVariables = true;
    options.includeAllJointTorquesAsSensors = true;
    ok = berdyHelper.init(estimator.model(), estimator.sensors(), options);
    ASSERT_IS_TRUE(ok);
    testBerdySensorMatrices(berdyHelper, fileName);

}

int main()
{
    for(unsigned int mdl = 0; mdl < 1; mdl++ )
    {
        std::string urdfFileName = getAbsModelPath(std::string(IDYNTREE_TESTS_URDFS[mdl]));
        std::cout << "BerdyHelperUnitTest, testing file " << std::string(IDYNTREE_TESTS_URDFS[mdl]) <<  std::endl;
        testBerdyHelpers(urdfFileName);
    }

    return EXIT_SUCCESS;
}

class Impl
{
public:
    Impl()
    {
        gravity.zero();
        gravity(2) = -9.81;
    }

    // Attached interfaces
    hde::interfaces::IHumanState* iHumanState = nullptr;
    hde::interfaces::IHumanWrench* iHumanWrench = nullptr;
    yarp::dev::IAnalogSensor* iAnalogSensor = nullptr;

    mutable std::mutex mutex;
    iDynTree::Vector3 gravity;

    const std::unordered_map<iDynTree::BerdySensorTypes, std::string> mapBerdySensorType = {
        {iDynTree::BerdySensorTypes::SIX_AXIS_FORCE_TORQUE_SENSOR, "SIX_AXIS_FORCE_TORQUE_SENSOR"},
        {iDynTree::BerdySensorTypes::ACCELEROMETER_SENSOR, "ACCELEROMETER_SENSOR"},
        {iDynTree::BerdySensorTypes::GYROSCOPE_SENSOR, "GYROSCOPE_SENSOR"},
        {iDynTree::BerdySensorTypes::THREE_AXIS_ANGULAR_ACCELEROMETER_SENSOR,
         "THREE_AXIS_ANGULAR_ACCELEROMETER_SENSOR"},
        {iDynTree::BerdySensorTypes::THREE_AXIS_FORCE_TORQUE_CONTACT_SENSOR,
         "THREE_AXIS_FORCE_TORQUE_CONTACT_SENSOR"},
        {iDynTree::BerdySensorTypes::DOF_ACCELERATION_SENSOR, "DOF_ACCELERATION_SENSOR"},
        {iDynTree::BerdySensorTypes::DOF_TORQUE_SENSOR, "DOF_TORQUE_SENSOR"},
        {iDynTree::BerdySensorTypes::NET_EXT_WRENCH_SENSOR, "NET_EXT_WRENCH_SENSOR"},
        {iDynTree::BerdySensorTypes::JOINT_WRENCH_SENSOR, "JOINT_WRENCH_SENSOR"}};

    // Berdy sensors map
    SensorMapIndex sensorMapIndex;

    // Berdy variable
    BerdyData berdyData;

    // Model variables
    iDynTree::Model humanModel;

    // Wrench sensor link names variable
    std::vector<std::string> wrenchSensorsLinkNames;
};
